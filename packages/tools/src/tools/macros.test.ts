import { describe, expect, it } from 'vitest';
import { newId, type Op } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { ToolContext } from '../registry.ts';
import { addCaptions } from './macros.ts';

/** Minimal ToolContext over an in-memory doc: just enough for the macro tools. */
function makeCtx(doc: ReturnType<typeof createEmptyDoc>, transcripts: unknown[]) {
  const state = { doc, applied: [] as Op[] };
  const ctx = {
    actor: 'builtin-agent',
    async getSnapshot() {
      return { doc: state.doc, assets: [], transcripts };
    },
    async applyOps(ops: Op[]) {
      state.applied.push(...ops);
      const r = applyOps(state.doc, ops);
      state.doc = r.doc;
      return { inverses: r.inverse, seq: 1 };
    },
  } as unknown as ToolContext;
  return { ctx, state };
}

describe('addCaptions', () => {
  it('stores word times relative to the caption, whatever the clip position or source offset', async () => {
    const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 });
    const mainTrack = doc0.tracks.find((t) => t.kind === 'video')!;
    // clip sits at timeline frame 300 and plays source from frame 900 (30s in)
    const clip = createItem('video', {
      id: newId('itm'),
      trackId: mainTrack.id,
      startFrame: 300,
      durationFrames: 300,
      assetId: 'ast_0000000000a',
      sourceInFrame: 900,
    } as never);
    const doc = applyOps(doc0, [{ type: 'item.add', item: clip }]).doc;
    const words = [
      { w: 'hello', startMs: 31000, endMs: 31400, conf: 1 },
      { w: 'brave', startMs: 31500, endMs: 31900, conf: 1 },
      { w: 'new', startMs: 32000, endMs: 32300, conf: 1 },
      { w: 'world', startMs: 32400, endMs: 32900, conf: 1 },
    ];
    const { ctx, state } = makeCtx(doc, [{ assetId: 'ast_0000000000a', words }]);

    await addCaptions.handler(addCaptions.input.parse({ wordsPerCard: 4 }), ctx);

    const caption = state.doc.items.find((i) => i.type === 'caption')!;
    expect(caption.startFrame).toBe(330); // 300 + (930 - 900)
    const props = caption.props as { words: { w: string; startMs: number; endMs: number }[] };
    expect(props.words[0]).toMatchObject({ w: 'hello', startMs: 0 });
    expect(props.words[1]!.startMs).toBe(500);
    // every word fits inside the caption's own duration
    const durationMs = (caption.durationFrames / 30) * 1000;
    expect(Math.max(...props.words.map((w) => w.startMs))).toBeLessThan(durationMs);
  });
});
