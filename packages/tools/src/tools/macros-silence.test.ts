import { describe, expect, it } from 'vitest';
import { newId, type Op } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { ToolContext } from '../registry.ts';
import { buildRoughCut, duckMusic, looksLikeSpeech, removeSilences } from './macros.ts';

const FPS = 30;
const w = (word: string, startMs: number, endMs: number) => ({ w: word, startMs, endMs });

/** Whisper-style words: every word's end is stretched to the next word's start, hiding the pause. */
const run1 = Array.from({ length: 12 }, (_, i) => w(`one${i}`, i * 333, (i + 1) * 333));
const run2 = Array.from({ length: 8 }, (_, i) => w(`two${i}`, 9000 + i * 330, 9000 + (i + 1) * 330));
// the last word of run 1 ends at 4000 + the pause (Whisper stretches word ends over silence)
const stretched = [...run1.slice(0, -1), w('one11', 3663, 9000), ...run2];

function setup(opts: { silences?: { startMs: number; endMs: number }[] | null; music?: boolean }) {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: FPS });
  const video = doc0.tracks.find((t) => t.kind === 'video')!;
  const audio = doc0.tracks.find((t) => t.kind === 'audio')!;
  const talk = createItem('video', { id: newId('itm'), trackId: video.id, startFrame: 0, durationFrames: 12 * FPS, assetId: 'ast_talk', sourceInFrame: 0 } as never);
  const items: unknown[] = [talk];
  let music;
  if (opts.music) {
    music = createItem('audio', { id: newId('itm'), trackId: audio.id, startFrame: 0, durationFrames: 12 * FPS, assetId: 'ast_music', sourceInFrame: 0, volume: 0.4 } as never);
    items.push(music);
  }
  const state = { doc: applyOps(doc0, items.map((item) => ({ type: 'item.add', item }) as Op)).doc };
  const transcripts = [
    { assetId: 'ast_talk', words: stretched },
    // Whisper on music: a couple of stray tokens
    { assetId: 'ast_music', words: [w('you', 1000, 1500), w('♪', 20000, 21000)] },
  ];
  const assets = [
    { id: 'ast_talk', kind: 'video', durationMs: 12000, hasSpeech: true },
    { id: 'ast_music', kind: 'audio', durationMs: 60000, hasSpeech: true },
  ];
  const ctx = {
    actor: 'builtin-agent',
    async getSnapshot() {
      return { doc: state.doc, assets, transcripts };
    },
    async applyOps(ops: Op[]) {
      const r = applyOps(state.doc, ops);
      state.doc = r.doc;
      return { inverses: r.inverse, seq: 1 };
    },
    ...(opts.silences === null ? {} : { async getSilences(id: string) { return id === 'ast_talk' ? opts.silences ?? [] : null; } }),
  } as unknown as ToolContext;
  return { ctx, state, talk, music };
}

const total = (state: { doc: { items: { startFrame: number; durationFrames: number }[] } }) => state.doc.items.reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);

describe('removeSilences from the audio', () => {
  const silences = [{ startMs: 4000, endMs: 8950 }];

  it('cuts a pause that the transcript word gaps do not show, leaving a handle', async () => {
    const { ctx, state } = setup({ silences });
    // word gaps are zero, so transcript-only logic would find nothing
    const r = (await removeSilences.handler(removeSilences.input.parse({}), ctx)) as { cutSeconds: number };
    // 4.95 s of silence minus a 0.1 s handle on each side
    expect(r.cutSeconds).toBeCloseTo(4.75, 1);
    expect(total(state)).toBeCloseTo((12 - 4.75) * FPS, -1);
  });

  it('honours the handle and the threshold', async () => {
    const a = setup({ silences });
    await removeSilences.handler(removeSilences.input.parse({ handleSec: 0.3 }), a.ctx);
    const d = setup({ silences });
    await removeSilences.handler(removeSilences.input.parse({}), d.ctx);
    expect(total(a.state) - total(d.state)).toBeCloseTo(0.4 * FPS, -1); // a bigger handle keeps more
    const b = setup({ silences: [{ startMs: 5000, endMs: 5400 }] });
    await expect(removeSilences.handler(removeSilences.input.parse({}), b.ctx)).rejects.toThrow(/No silences/);
  });

  it('trusts the audio over ASR word times that stretch into the pause', async () => {
    // "one11" is reported as ending at 9.0 s and a stretched word may even start inside the silence: still cut the whole pause
    const { ctx, state } = setup({ silences: [{ startMs: 4000, endMs: 8950 }] });
    const snap = (await ctx.getSnapshot()) as { transcripts: { words: { w: string; startMs: number; endMs: number }[] }[] };
    snap.transcripts[0]!.words.splice(11, 1, w('one11', 4200, 8950));
    await removeSilences.handler(removeSilences.input.parse({}), ctx);
    expect(total(state)).toBeCloseTo((12 - 4.75) * FPS, -1);
  });

  it('falls back to transcript gaps when no silence map exists', async () => {
    const { ctx } = setup({ silences: null });
    const words = [w('a', 0, 400), w('b', 2000, 2400)];
    const s = (await ctx.getSnapshot()) as { transcripts: { assetId: string; words: unknown[] }[] };
    s.transcripts[0]!.words = words;
    const r = (await removeSilences.handler(removeSilences.input.parse({}), ctx)) as { applied: boolean };
    expect(r.applied).toBe(true);
  });

  it('buildRoughCut uses the same audio pauses', async () => {
    const { ctx, state } = setup({ silences });
    await buildRoughCut.handler(buildRoughCut.input.parse({}), ctx);
    expect(total(state)).toBeLessThan(9 * FPS);
  });
});

describe('duckMusic', () => {
  const silences = [{ startMs: 4000, endMs: 8950 }];
  const run = async (opts: Parameters<typeof setup>[0], input: object = {}) => {
    const s = setup({ music: true, ...opts });
    const r = await duckMusic.handler(duckMusic.input.parse({ musicItemId: s.music!.id, ...input }), s.ctx);
    const kf = s.state.doc.items.find((i) => i.id === s.music!.id)!.keyframes['volume'] ?? [];
    return { r, kf };
  };
  const at = (kf: { frame: number; value: number }[], sec: number) => {
    const f = sec * FPS;
    for (let i = 1; i < kf.length; i++) {
      if (f <= kf[i]!.frame) {
        const a = kf[i - 1]!;
        const b = kf[i]!;
        return a.value + ((b.value - a.value) * (f - a.frame)) / Math.max(1, b.frame - a.frame);
      }
    }
    return kf[kf.length - 1]!.value;
  };

  it('ducks under speech only, comes back up in the long pause, relative to the item volume', async () => {
    const { kf } = await run({ silences });
    expect(at(kf, 2)).toBeCloseTo(0.1, 2); // 0.4 * 0.25 under "hello"
    expect(at(kf, 6.5)).toBeCloseTo(0.4, 2); // pause: back to the item's own volume, not 1
    expect(at(kf, 9.2)).toBeCloseTo(0.1, 2); // "again"
    expect(Math.max(...kf.map((k) => k.value))).toBeCloseTo(0.4, 5);
  });

  it('uses short ramps', async () => {
    const { kf } = await run({ silences });
    const drop = kf.findIndex((k, i) => i > 0 && k.value < kf[i - 1]!.value);
    expect(kf[drop]!.frame - kf[drop - 1]!.frame).toBeLessThanOrEqual(Math.round(0.15 * FPS) + 1);
  });

  it('does not treat the music asset as speech', async () => {
    // only a music-like transcript is available: nothing to duck under
    const s = setup({ music: true, silences });
    const snap = (await s.ctx.getSnapshot()) as { doc: typeof s.state.doc; transcripts: { assetId: string; words: unknown[] }[] };
    expect(looksLikeSpeech([w('you', 1000, 1500), w('♪', 20000, 21000)], 60000)).toBe(false);
    expect(looksLikeSpeech(Array.from({ length: 30 }, (_, i) => w('thanks', i * 1000, i * 1000 + 500)), 30000)).toBe(false);
    expect(looksLikeSpeech(stretched, 12000)).toBe(true);
    expect(looksLikeSpeech([w('a', 0, 300), w('b', 400, 700), w('c', 800, 900)], 60000)).toBe(false); // 3 words in a minute
    void snap;
  });
});
