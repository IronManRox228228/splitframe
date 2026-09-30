import { describe, expect, it } from 'vitest';
import { newId, Item } from '@cutboard/schema';
import { applyOp, applyOps, makeDoc, videoItem } from './test-helpers.ts';
import { History } from './history.ts';

const byId = (items: Item[]) => [...items].sort((x, y) => x.id.localeCompare(y.id));

describe('ripple delete', () => {
  it('closes the gap and fully restores on undo', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 60);
    const b = videoItem(doc, 60, 60);
    const c = videoItem(doc, 120, 60);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
      { type: 'item.add', item: c },
    ]);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.remove',
      itemIds: [b.id],
      ripple: true,
    });
    expect(d2.items).toHaveLength(2);
    expect(d2.items.find((i) => i.id === c.id)!.startFrame).toBe(60);
    const { doc: d3 } = applyOps(d2, inverse);
    // item array order is not semantic; compare as sorted sets
    expect(byId(d3.items)).toEqual(byId(d1.items));
  });

  it('multi-item ripple delete on one track', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 30);
    const b = videoItem(doc, 30, 30);
    const c = videoItem(doc, 60, 30);
    const d = videoItem(doc, 90, 30);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
      { type: 'item.add', item: c },
      { type: 'item.add', item: d },
    ]);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.remove',
      itemIds: [b.id, c.id],
      ripple: true,
    });
    expect(d2.items.find((i) => i.id === d.id)!.startFrame).toBe(30);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(byId(d3.items)).toEqual(byId(d1.items));
  });

  it('non-ripple remove leaves downstream items alone', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 60);
    const b = videoItem(doc, 60, 60);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
    ]);
    const { doc: d2 } = applyOp(d1, {
      type: 'item.remove',
      itemIds: [a.id],
      ripple: false,
    });
    expect(d2.items.find((i) => i.id === b.id)!.startFrame).toBe(60);
  });
});

describe('History', () => {
  it('undo/redo walks the stack in order', () => {
    const h = new History();
    expect(h.canUndo).toBe(false);
    h.push([{ type: 'project.rename', name: 'a' }], [{ type: 'project.rename', name: 'x' }]);
    h.push([{ type: 'project.rename', name: 'b' }], [{ type: 'project.rename', name: 'a' }]);
    expect(h.canUndo).toBe(true);
    expect(h.undo()!.ops[0]).toEqual({ type: 'project.rename', name: 'b' });
    expect(h.undo()!.ops[0]).toEqual({ type: 'project.rename', name: 'a' });
    expect(h.canUndo).toBe(false);
    expect(h.redo()!.ops[0]).toEqual({ type: 'project.rename', name: 'a' });
    expect(h.redo()!.ops[0]).toEqual({ type: 'project.rename', name: 'b' });
    expect(h.canRedo).toBe(false);
  });

  it('a new push discards the redo tail', () => {
    const h = new History();
    h.push([{ type: 'project.rename', name: 'a' }], [{ type: 'project.rename', name: 'x' }]);
    h.push([{ type: 'project.rename', name: 'b' }], [{ type: 'project.rename', name: 'a' }]);
    h.undo();
    h.push([{ type: 'project.rename', name: 'c' }], [{ type: 'project.rename', name: 'x' }]);
    expect(h.canRedo).toBe(false);
    expect(h.undo()!.ops[0]).toEqual({ type: 'project.rename', name: 'c' });
    expect(h.undo()!.ops[0]).toEqual({ type: 'project.rename', name: 'a' });
  });

  it('groups agent turns: beginGroup → pushes → commitGroup is one undo', () => {
    const h = new History();
    h.beginGroup('Agent turn');
    h.push([{ type: 'project.rename', name: 'a' }], [{ type: 'project.rename', name: 'x' }]);
    h.push([{ type: 'project.rename', name: 'b' }], [{ type: 'project.rename', name: 'a' }]);
    const group = h.commitGroup();
    expect(group!.label).toBe('Agent turn');
    expect(group!.ops).toHaveLength(2);
    expect(h.undo()!.ops).toHaveLength(2);
    expect(h.canUndo).toBe(false);
  });

  it('uncommitted empty groups are dropped', () => {
    const h = new History();
    h.beginGroup();
    expect(h.commitGroup()).toBeNull();
    expect(h.canUndo).toBe(false);
  });

  it('respects the size limit', () => {
    const h = new History(3);
    for (let i = 0; i < 5; i++) {
      h.push([{ type: 'project.rename', name: `n${i}` }], [{ type: 'project.rename', name: `p${i}` }]);
    }
    let undos = 0;
    while (h.undo()) undos++;
    expect(undos).toBe(3);
  });
});

describe('undo roundtrip over a full edit sequence', () => {
  it('a realistic session replays back to the empty doc', () => {
    const doc = makeDoc();
    const ops: Parameters<typeof applyOps>[1] = [];
    const item = videoItem(doc, 0, 120);
    ops.push({ type: 'item.add', item });
    ops.push({ type: 'item.trim', itemId: item.id, edge: 'out', frame: 90, ripple: false });
    const splitId = newId('itm');
    ops.push({ type: 'item.split', itemId: item.id, atFrame: 45, newItemId: splitId });
    ops.push({ type: 'item.update', itemId: splitId, patch: { volume: 0.5 } });
    ops.push({
      type: 'marker.add',
      marker: { id: newId('mrk'), frame: 30, label: 'start' },
    });
    const { doc: after, inverse } = applyOps(doc, ops);
    expect(after.items).toHaveLength(2);
    const { doc: restored } = applyOps(after, inverse);
    expect(restored.items).toEqual(doc.items);
    expect(restored.markers).toEqual(doc.markers);
    expect(restored.tracks).toEqual(doc.tracks);
  });
});
