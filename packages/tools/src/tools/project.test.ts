import { describe, expect, it } from 'vitest';
import { newId, type Op } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { ToolContext } from '../registry.ts';
import { createToolRegistry } from '../index.ts';
import { canvasForAspect, setProjectSettings, undo, redo } from './project.ts';

function makeCtx() {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 });
  const track = doc0.tracks.find((t) => t.kind === 'video')!;
  const clip = createItem('video', { id: newId('itm'), trackId: track.id, startFrame: 30, durationFrames: 90, assetId: 'ast_a', sourceInFrame: 15, transform: { x: 100, y: -200, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 } } as never);
  const state = { doc: applyOps(doc0, [{ type: 'item.add', item: clip }]).doc, calls: [] as string[] };
  const ctx = {
    actor: 'builtin-agent',
    async getSnapshot() { return { doc: state.doc, assets: [], transcripts: [] }; },
    async applyOps(ops: Op[]) { const r = applyOps(state.doc, ops); state.doc = r.doc; return { inverses: r.inverse, seq: 1 }; },
    async undo(steps: number) { state.calls.push(`undo ${steps}`); return { steps, labels: ['x'], canUndo: true, canRedo: true }; },
    async redo(steps: number) { state.calls.push(`redo ${steps}`); return { steps: 0, labels: [], canUndo: true, canRedo: false }; },
  } as unknown as ToolContext;
  return { ctx, state };
}

describe('setProjectSettings', () => {
  it('9:16 is 1080x1920 and offsets scale with the canvas', async () => {
    expect(canvasForAspect('9:16', 1080)).toEqual({ width: 1080, height: 1920 });
    expect(canvasForAspect('16:9', 720)).toEqual({ width: 1280, height: 720 });
    const { ctx, state } = makeCtx();
    await setProjectSettings.handler(setProjectSettings.input.parse({ aspect: '9:16' }), ctx);
    expect(state.doc.project).toMatchObject({ width: 1080, height: 1920 });
    expect(state.doc.items[0]!.transform).toMatchObject({ x: Math.round(100 * (1080 / 1920)), y: Math.round(-200 * (1920 / 1080)) });
  });

  it('fps change keeps cuts at the same time in seconds, and one op undoes it exactly', async () => {
    const { ctx, state } = makeCtx();
    const before = JSON.parse(JSON.stringify(state.doc));
    await setProjectSettings.handler(setProjectSettings.input.parse({ fps: 60 }), ctx);
    const it = state.doc.items[0]!;
    expect([state.doc.project.fps, it.startFrame, it.durationFrames, it.sourceInFrame]).toEqual([60, 60, 180, 30]);
    // inverse restores the old timeline exactly
    const r = applyOps(before, [{ type: 'project.setFps', fps: 60 }]);
    expect(applyOps(r.doc, r.inverse).doc).toEqual(before);
  });

  it('rejects an empty or half-given request', async () => {
    const { ctx } = makeCtx();
    await expect(setProjectSettings.handler(setProjectSettings.input.parse({}), ctx)).rejects.toThrow(/Nothing to change/);
    await expect(setProjectSettings.handler(setProjectSettings.input.parse({ width: 1000 }), ctx)).rejects.toThrow(/both width and height/);
  });
});

describe('undo / redo tools', () => {
  it('are registered and pass the step count to the history', async () => {
    const reg = createToolRegistry();
    expect(reg.get('undo')).toBeDefined();
    expect(reg.get('redo')).toBeDefined();
    const { ctx, state } = makeCtx();
    const r = (await undo.handler(undo.input.parse({ steps: 2 }), ctx)) as { steps: number };
    expect(r.steps).toBe(2);
    const r2 = (await redo.handler(redo.input.parse({}), ctx)) as { note: string };
    expect(state.calls).toEqual(['undo 2', 'redo 1']);
    expect(r2.note).toMatch(/Nothing to redo/);
  });
});
