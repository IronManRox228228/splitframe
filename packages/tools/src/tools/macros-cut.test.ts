import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem, docDurationFrames } from '@cutboard/editor-core';
import { cutSpansOps } from './macros.ts';

function setup(duration = 300) {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 });
  const track = doc0.tracks.find((t) => t.kind === 'video')!;
  const clip = createItem('video', {
    id: newId('itm'),
    trackId: track.id,
    startFrame: 0,
    durationFrames: duration,
    assetId: 'ast_0000000000a',
    sourceInFrame: 0,
  } as never);
  const doc = applyOps(doc0, [{ type: 'item.add', item: clip }]).doc;
  return { doc, clip };
}

const layout = (doc: ReturnType<typeof setup>['doc']) =>
  [...doc.items].sort((a, b) => a.startFrame - b.startFrame).map((i) => [i.startFrame, i.durationFrames, i.sourceInFrame]);

describe('cutSpansOps', () => {
  it('cuts disjoint spans and closes the gaps', () => {
    const { doc, clip } = setup();
    const ops = cutSpansOps(clip, [
      { startFrame: 60, endFrame: 90 },
      { startFrame: 200, endFrame: 230 },
    ]);
    const out = applyOps(doc, ops).doc;
    expect(layout(out)).toEqual([
      [0, 60, 0],
      [60, 110, 90],
      [170, 70, 230],
    ]);
    expect(docDurationFrames(out)).toBe(240);
  });

  it('merges overlapping spans (a pause inside a repeated phrase) instead of failing', () => {
    const { doc, clip } = setup();
    const ops = cutSpansOps(clip, [
      { startFrame: 100, endFrame: 200 }, // retake
      { startFrame: 140, endFrame: 160 }, // pause inside it
    ]);
    const out = applyOps(doc, ops).doc;
    expect(layout(out)).toEqual([
      [0, 100, 0],
      [100, 100, 200],
    ]);
  });

  it('removes a leading span without leaving a one-frame stub', () => {
    const { doc, clip } = setup();
    const out = applyOps(doc, cutSpansOps(clip, [{ startFrame: 0, endFrame: 40 }])).doc;
    expect(layout(out)).toEqual([[0, 260, 40]]);
  });

  it('removes a trailing span and a span covering the whole item', () => {
    const { doc, clip } = setup();
    expect(layout(applyOps(doc, cutSpansOps(clip, [{ startFrame: 250, endFrame: 300 }])).doc)).toEqual([[0, 250, 0]]);
    expect(applyOps(doc, cutSpansOps(clip, [{ startFrame: 0, endFrame: 300 }])).doc.items).toHaveLength(0);
  });
});
