import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOp, applyOps, makeDoc, trackOfKind, videoItem } from './test-helpers.ts';
import { History } from './history.ts';

function docWith(doc: ReturnType<typeof makeDoc>, ...items: ReturnType<typeof videoItem>[]) {
  return applyOps(doc, items.map((item) => ({ type: 'item.add' as const, item }))).doc;
}

describe('trim boundaries', () => {
  it('never moves the start below frame 0 when extending the head', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 10, 100, { sourceInFrame: 100 });
    const d1 = docWith(doc, item);
    const { doc: d2 } = applyOp(d1, { type: 'item.trim', itemId: item.id, edge: 'in', frame: -50, ripple: false });
    const t = d2.items[0]!;
    expect(t.startFrame).toBe(0);
    expect(t.startFrame + t.durationFrames).toBe(110); // the tail stays put
    expect(t.sourceInFrame).toBe(90); // head extended by 10 frames only
  });

  it('keeps the start inside the item when the in edge is dragged past the end', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 10, 100);
    const d1 = docWith(doc, item);
    const { doc: d2 } = applyOp(d1, { type: 'item.trim', itemId: item.id, edge: 'in', frame: 500, ripple: false });
    const t = d2.items[0]!;
    expect(t.startFrame).toBe(109);
    expect(t.durationFrames).toBe(1);
    expect(t.sourceInFrame).toBe(99);
  });

  it('never slides downstream items by more than the item length on ripple trim-in', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 100);
    const b = videoItem(doc, 100, 50);
    const d1 = docWith(doc, a, b);
    const { doc: d2 } = applyOp(d1, { type: 'item.trim', itemId: a.id, edge: 'in', frame: 500, ripple: true });
    expect(d2.items.find((i) => i.id === a.id)!.durationFrames).toBe(1);
    expect(d2.items.find((i) => i.id === b.id)!.startFrame).toBe(1); // 100 - 99
  });

  it('restores the exact geometry when undoing an in-trim at a fractional speed', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 100, { speed: 0.4 });
    const d1 = docWith(doc, item);
    const { doc: d2, inverse } = applyOp(d1, { type: 'item.trim', itemId: item.id, edge: 'in', frame: 3, ripple: false });
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });
});

describe('item.clone', () => {
  it('rejects a destination track that cannot hold the item type', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 100);
    const d1 = docWith(doc, item);
    expect(() =>
      applyOp(d1, { type: 'item.clone', itemId: item.id, newItemId: newId('itm'), trackId: trackOfKind(d1, 'audio').id }),
    ).toThrow(/cannot live on/i);
  });
});

describe('locked tracks', () => {
  function locked() {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 100);
    const d1 = docWith(doc, item);
    const { doc: d2 } = applyOp(d1, { type: 'track.update', trackId: item.trackId, patch: { locked: true } });
    return { item, doc: d2 };
  }

  it('rejects edits to items on a locked track', () => {
    const { item, doc } = locked();
    const ops = [
      { type: 'item.move', itemId: item.id, startFrame: 5 },
      { type: 'item.split', itemId: item.id, atFrame: 50, newItemId: newId('itm') },
      { type: 'item.remove', itemIds: [item.id], ripple: false },
      { type: 'item.update', itemId: item.id, patch: { volume: 0.5 } },
      { type: 'item.clone', itemId: item.id, newItemId: newId('itm') },
      { type: 'item.setSpeed', itemId: item.id, speed: 2 },
    ] as const;
    for (const op of ops) expect(() => applyOp(doc, op as never), op.type).toThrow(/locked/i);
  });

  it('rejects adding to a locked track and removing it', () => {
    const { item, doc } = locked();
    const other = videoItem(doc, 200, 10, { trackId: item.trackId });
    expect(() => applyOp(doc, { type: 'item.add', item: other })).toThrow(/locked/i);
    expect(() => applyOp(doc, { type: 'track.remove', trackId: item.trackId })).toThrow(/locked/i);
  });

  it('still allows unlocking, and undo/redo may bypass the lock', () => {
    const { item, doc } = locked();
    const { doc: unlocked } = applyOp(doc, { type: 'track.update', trackId: item.trackId, patch: { locked: false } });
    expect(() => applyOp(unlocked, { type: 'item.move', itemId: item.id, startFrame: 5 })).not.toThrow();
    expect(() => applyOp(doc, { type: 'item.move', itemId: item.id, startFrame: 5 }, { enforceLocks: false })).not.toThrow();
  });

  it('checks each op of a batch against the evolving doc', () => {
    const { item, doc } = locked();
    const { doc: after } = applyOps(doc, [
      { type: 'track.update', trackId: item.trackId, patch: { locked: false } },
      { type: 'item.move', itemId: item.id, startFrame: 5 },
    ]);
    expect(after.items[0]!.startFrame).toBe(5);
  });
});

describe('inverse fidelity', () => {
  it('removes label keys that did not exist before the update', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 100);
    const d1 = docWith(doc, item);
    const { doc: d2, inverse } = applyOp(d1, { type: 'item.update', itemId: item.id, patch: { labels: { name: 'hello' } } });
    expect(d2.items[0]!.labels.name).toBe('hello');
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]!.labels).toEqual({});
  });

  it('removes keyframe tracks that did not exist before the update', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 100);
    const d1 = docWith(doc, item);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.update',
      itemId: item.id,
      patch: { keyframes: { 'transform.x': [{ frame: 0, value: 1, easing: 'linear' }] } },
    });
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]!.keyframes).toEqual({});
  });
});

describe('undo groups', () => {
  it('undoes a multi-push group newest-first', () => {
    const doc = makeDoc();
    const a = applyOp(doc, { type: 'project.rename', name: 'A' });
    const b = applyOp(a.doc, { type: 'project.rename', name: 'B' });
    const h = new History();
    h.beginGroup('turn');
    h.push([{ type: 'project.rename', name: 'A' }], a.inverse);
    h.push([{ type: 'project.rename', name: 'B' }], b.inverse);
    const group = h.undo()!;
    const { doc: undone } = applyOps(b.doc, group.inverses);
    expect(undone.project.name).toBe(doc.project.name);
  });
});
