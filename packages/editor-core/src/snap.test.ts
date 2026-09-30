import { describe, expect, it } from 'vitest';
import { applyOp, applyOps, makeDoc, videoItem } from './test-helpers.ts';
import { getSnapCandidates, snapFrame, snapItemStart } from './snap.ts';

function docWithItems() {
  const doc = makeDoc();
  const a = videoItem(doc, 0, 60);
  const b = videoItem(doc, 90, 60);
  const { doc: d1 } = applyOps(doc, [
    { type: 'item.add', item: a },
    { type: 'item.add', item: b },
  ]);
  return { doc: d1, a, b };
}

describe('snapping', () => {
  it('collects item edges and zero', () => {
    const { doc } = docWithItems();
    const candidates = getSnapCandidates(doc);
    const frames = candidates.map((c) => c.frame).sort((x, y) => x - y);
    expect(frames).toEqual([0, 0, 60, 90, 150]);
  });

  it('excludes the dragged item from its own candidates', () => {
    const { doc, b } = docWithItems();
    const candidates = getSnapCandidates(doc, { excludeItemIds: [b.id] });
    expect(candidates.filter((c) => c.source === b.id)).toHaveLength(0);
  });

  it('snaps to the nearest candidate within threshold', () => {
    const { doc } = docWithItems();
    const candidates = getSnapCandidates(doc);
    expect(snapFrame(62, candidates, 5)!.frame).toBe(60);
    expect(snapFrame(88, candidates, 5)!.frame).toBe(90);
    expect(snapFrame(80, candidates, 5)).toBeNull();
  });

  it('snapItemStart considers both edges of the moving item', () => {
    const { doc, b } = docWithItems();
    // dragging b so its OUT edge lands on 60 (a's end) → start = 0
    expect(snapItemStart(doc, b.id, 2, 5)).toBe(0);
    // dragging b so its IN edge lands on 60 → start = 60
    expect(snapItemStart(doc, b.id, 58, 5)).toBe(60);
    // no candidate nearby → unchanged
    expect(snapItemStart(doc, b.id, 45, 3)).toBe(45);
  });

  it('includes markers and the playhead when asked', () => {
    const { doc } = docWithItems();
    const { doc: d1 } = applyOp(doc, {
      type: 'marker.add',
      marker: { id: 'mrk_test', frame: 45, label: 'hit' },
    });
    const candidates = getSnapCandidates(d1, { includePlayhead: 47 });
    const frames = candidates.map((c) => c.frame);
    expect(frames).toContain(45);
    expect(frames).toContain(47);
  });
});
