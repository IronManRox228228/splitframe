import { describe, expect, it } from 'vitest';
import { newId, type Op } from '@cutboard/schema';
import { applyOps, createEmptyDoc } from '@cutboard/editor-core';
import type { ToolContext } from '../registry.ts';
import { createToolRegistry } from '../index.ts';
import { addAudio, addClip, addText, updateItem } from './edit.ts';
import { beatSync } from './beat.ts';

interface FakeAsset {
  id: string;
  kind: 'video' | 'audio' | 'image';
  durationMs: number;
  originalName?: string;
}

/** Minimal ToolContext over an in-memory doc: just enough for the edit/beat tools. */
function makeCtx(assets: FakeAsset[], beatsMs: number[] = []) {
  const state = { doc: createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 }) };
  const ctx = {
    actor: 'builtin-agent',
    async getSnapshot() {
      return { doc: state.doc, assets, transcripts: [] };
    },
    async applyOps(ops: Op[]) {
      const r = applyOps(state.doc, ops);
      state.doc = r.doc;
      return { inverses: r.inverse, seq: 1 };
    },
    async analyzeBeats() {
      return { bpm: 120, beatsMs, downbeatsMs: [], sections: [{ startMs: 0, endMs: 600000, label: 'high', energy: 1 }] };
    },
  } as unknown as ToolContext;
  return { ctx, state };
}

describe('addText', () => {
  it('fills in the default style when none is given', async () => {
    const { ctx, state } = makeCtx([]);
    await addText.handler(addText.input.parse({ text: 'Hello', startFrame: 0, durationFrames: 60 }), ctx);
    expect(state.doc.items[0]).toMatchObject({ type: 'text', props: { text: 'Hello', style: { fontFamily: 'Geist', fontSize: 72 } } });
  });
});

describe('addClip', () => {
  it('defaults to the whole asset instead of one second', async () => {
    const { ctx, state } = makeCtx([{ id: 'ast_v', kind: 'video', durationMs: 12_000 }]);
    await addClip.handler(addClip.input.parse({ assetId: 'ast_v' }), ctx);
    expect(state.doc.items[0]!.durationFrames).toBe(360);
  });

  it('takes sourceIn into account and gives stills five seconds', async () => {
    const { ctx, state } = makeCtx([
      { id: 'ast_v', kind: 'video', durationMs: 10_000 },
      { id: 'ast_i', kind: 'image', durationMs: 0 },
    ]);
    await addClip.handler(addClip.input.parse({ assetId: 'ast_v', sourceInFrame: 60 }), ctx);
    await addClip.handler(addClip.input.parse({ assetId: 'ast_i' }), ctx);
    const [video, image] = state.doc.items;
    expect(video!.durationFrames).toBe(240);
    expect(image!.durationFrames).toBe(150);
  });

  it('puts an audio asset on the audio track instead of failing on the video track', async () => {
    const { ctx, state } = makeCtx([{ id: 'ast_a', kind: 'audio', durationMs: 4000 }]);
    await addClip.handler(addClip.input.parse({ assetId: 'ast_a' }), ctx);
    const audioTrack = state.doc.tracks.find((t) => t.kind === 'audio')!;
    expect(state.doc.items[0]).toMatchObject({ type: 'audio', trackId: audioTrack.id, durationFrames: 120 });
  });
});

describe('addAudio', () => {
  it('defaults to the full asset length', async () => {
    const { ctx, state } = makeCtx([{ id: 'ast_a', kind: 'audio', durationMs: 42_000 }]);
    await addAudio.handler(addAudio.input.parse({ assetId: 'ast_a' }), ctx);
    expect(state.doc.items[0]!.durationFrames).toBe(1260);
  });
});

describe('registry', () => {
  it('names the available tools when asked for an unknown one', async () => {
    const registry = createToolRegistry();
    await expect(registry.call('nope', {}, {} as ToolContext)).rejects.toThrow(/Available tools: .*addClip/);
  });
});

describe('beatSync', () => {
  const assets: FakeAsset[] = [
    { id: 'ast_m', kind: 'audio', durationMs: 20_000 },
    { id: 'ast_v1', kind: 'video', durationMs: 3_000, originalName: 'a.mp4' },
  ];

  async function withMusic(sourceInFrame: number, beatsMs: number[], extra: Record<string, unknown> = {}) {
    const made = makeCtx(assets, beatsMs);
    const audioTrack = made.state.doc.tracks.find((t) => t.kind === 'audio')!;
    await made.ctx.applyOps(
      [
        {
          type: 'item.add',
          item: {
            id: 'itm_music000000a',
            trackId: audioTrack.id,
            type: 'audio',
            startFrame: 0,
            durationFrames: 600,
            assetId: 'ast_m',
            sourceInFrame,
            speed: 1,
            timeRemap: [],
            transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
            volume: 1,
            muted: false,
            effects: [],
            masks: [],
            keyframes: {},
            labels: {},
            props: { fadeInFrames: 0, fadeOutFrames: 0 },
          },
        } as never,
      ],
      'builtin-agent',
    );
    const result = await beatSync.handler(beatSync.input.parse({ musicItemId: 'itm_music000000a', ...extra }), made.ctx);
    return { ...made, result };
  }

  it('ignores beats before the music is trimmed in instead of placing clips at negative frames', async () => {
    // music trimmed 3s in (sourceIn 90 frames): the beats at 0-2.5s never play
    const beats = Array.from({ length: 20 }, (_, i) => i * 500);
    const { state } = await withMusic(90, beats);
    const clips = state.doc.items.filter((i) => i.type === 'video');
    expect(clips.length).toBeGreaterThan(0);
    for (const clip of clips) expect(clip.startFrame).toBeGreaterThanOrEqual(0);
  });

  it('holds the last clip through the remaining beats when footage runs out', async () => {
    const beats = Array.from({ length: 21 }, (_, i) => i * 500); // 10s of beats, only 3s of footage
    const { state } = await withMusic(0, beats, { density: { high: 1, medium: 1, low: 1 } });
    const clips = state.doc.items.filter((i) => i.type === 'video').sort((a, b) => a.startFrame - b.startFrame);
    const last = clips[clips.length - 1]!;
    expect(last.startFrame + last.durationFrames).toBeGreaterThanOrEqual(300);
  });
});

describe('edit tool results', () => {
  it('report the item they changed, so the agent does not think the edit failed', async () => {
    const { ctx, state } = makeCtx([{ id: 'ast_a', kind: 'audio', durationMs: 3000 }]);
    const added = (await addAudio.handler(addAudio.input.parse({ assetId: 'ast_a', startFrame: 60 }), ctx)) as { items: { id: string }[]; itemCount: number };
    expect(added.itemCount).toBe(1);
    expect(added.items.map((i) => i.id)).toEqual([state.doc.items[0]!.id]);
    const updated = (await updateItem.handler(updateItem.input.parse({ itemId: state.doc.items[0]!.id, patch: { muted: true } }), ctx)) as { items: { muted?: boolean }[] };
    expect(updated.items[0]!.muted).toBe(true);
  });
});
