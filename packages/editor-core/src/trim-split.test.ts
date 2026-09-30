import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOp, applyOps, makeDoc, videoItem, audioItem } from './test-helpers.ts';
import { itemEnd, sourceOutFrame } from './timeline-doc.ts';

describe('trim (plain)', () => {
  it('trims the in edge: start moves, source advances, end stays', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 30, 90, { sourceInFrame: 100 });
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.trim',
      itemId: item.id,
      edge: 'in',
      frame: 60,
      ripple: false,
    });
    const t = d2.items[0]!;
    expect(t.startFrame).toBe(60);
    expect(t.durationFrames).toBe(60); // end stays at 120
    expect(t.sourceInFrame).toBe(130); // advanced 30 frames * speed 1
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('trims the out edge: duration shrinks, start stays', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 30, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.trim',
      itemId: item.id,
      edge: 'out',
      frame: 90,
      ripple: false,
    });
    const t = d2.items[0]!;
    expect(t.startFrame).toBe(30);
    expect(t.durationFrames).toBe(60);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('rejects a no-op trim', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 30, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    expect(() =>
      applyOp(d1, { type: 'item.trim', itemId: item.id, edge: 'out', frame: 120, ripple: false }),
    ).toThrow(/would not change/i);
  });

  it('respects speed when advancing sourceIn', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 60, { sourceInFrame: 0, speed: 0.5 }); // slow-mo
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2 } = applyOp(d1, {
      type: 'item.trim',
      itemId: item.id,
      edge: 'in',
      frame: 30,
      ripple: false,
    });
    expect(d2.items[0]!.sourceInFrame).toBe(15); // 30 timeline frames * 0.5
  });

  it('clamps sourceIn at zero when extending the head', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 50, 50, { sourceInFrame: 10 });
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2 } = applyOp(d1, {
      type: 'item.trim',
      itemId: item.id,
      edge: 'in',
      frame: 0,
      ripple: false,
    });
    const t = d2.items[0]!;
    expect(t.sourceInFrame).toBe(0); // can't go below source 0
    expect(t.startFrame).toBe(40); // start adjusted to match available source
    expect(t.durationFrames).toBe(60);
  });
});

describe('trim (ripple)', () => {
  it('shortening the out edge pulls downstream items left', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 90);
    const b = videoItem(doc, 90, 90);
    const c = videoItem(doc, 180, 90);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
      { type: 'item.add', item: c },
    ]);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.trim',
      itemId: b.id,
      edge: 'out',
      frame: 150, // shorten by 30
      ripple: true,
    });
    expect(d2.items.find((i) => i.id === b.id)!.durationFrames).toBe(60);
    expect(d2.items.find((i) => i.id === c.id)!.startFrame).toBe(150);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items).toEqual(d1.items);
  });

  it('ripple trim-in anchors the item start and slides downstream', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 90);
    const b = videoItem(doc, 90, 90);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
    ]);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.trim',
      itemId: b.id,
      edge: 'in',
      frame: 120, // cut 30 frames off the head of b
      ripple: true,
    });
    const bb = d2.items.find((i) => i.id === b.id)!;
    expect(bb.startFrame).toBe(90); // anchored
    expect(bb.durationFrames).toBe(60);
    expect(bb.sourceInFrame).toBe(30);
    expect(d2.items.find((i) => i.id === a.id)!.startFrame).toBe(0);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items).toEqual(d1.items);
  });

  it('ripple extending the out edge pushes downstream right', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 0, 90);
    const b = videoItem(doc, 90, 90);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
    ]);
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.trim',
      itemId: a.id,
      edge: 'out',
      frame: 120,
      ripple: true,
    });
    expect(itemEnd(d2.items.find((i) => i.id === a.id)!)).toBe(120);
    expect(d2.items.find((i) => i.id === b.id)!.startFrame).toBe(120);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items).toEqual(d1.items);
  });
});

describe('split', () => {
  it('splits into two items with correct source and remap, and inverts exactly', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 10, 100, {
      sourceInFrame: 200,
      timeRemap: [
        { frame: 0, sourceFrame: 200 },
        { frame: 50, sourceFrame: 400 },
        { frame: 100, sourceFrame: 800 },
      ],
      keyframes: {
        'transform.scale': [
          { frame: 10, value: 1, easing: 'linear' },
          { frame: 60, value: 2, easing: 'linear' },
        ],
      },
    });
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const newIdB = newId('itm');
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.split',
      itemId: item.id,
      atFrame: 60,
      newItemId: newIdB,
    });
    const a = d2.items.find((i) => i.id === item.id)!;
    const b = d2.items.find((i) => i.id === newIdB)!;
    expect(a.durationFrames).toBe(50);
    expect(b.startFrame).toBe(60);
    expect(b.durationFrames).toBe(50);
    // sourceIn for the right part = sourceIn + splitLocal * speed = 200 + 50
    expect(b.sourceInFrame).toBe(250);
    expect(a.timeRemap.map((p) => p.frame)).toEqual([0]);
    expect(b.timeRemap.map((p) => p.frame)).toEqual([0, 50]);
    // keyframe at local 60 moves into b at 60 - 50 = 10
    expect(b.keyframes['transform.scale']!.map((k) => k.frame)).toEqual([10]);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items).toEqual(d1.items);
  });

  it('rejects splits outside the item', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 10, 100);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    expect(() =>
      applyOp(d1, { type: 'item.split', itemId: item.id, atFrame: 200, newItemId: newId('itm') }),
    ).toThrow(/outside/i);
  });
});

describe('slip / speed / remap ops', () => {
  it('slip changes sourceIn only and inverts', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90, { sourceInFrame: 0 });
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.slip',
      itemId: item.id,
      sourceInFrame: 40,
    });
    expect(d2.items[0]!.sourceInFrame).toBe(40);
    expect(d2.items[0]!.startFrame).toBe(0);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('audio counts as a media item and can slip', () => {
    const doc = makeDoc();
    const item = audioItem(doc, 0, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2 } = applyOp(d1, {
      type: 'item.slip',
      itemId: item.id,
      sourceInFrame: 5,
    });
    expect(d2.items[0]!.sourceInFrame).toBe(5);
  });

  it('setSpeed keeps duration, clears remap, and restores both on undo', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 60, {
      sourceInFrame: 0,
      timeRemap: [
        { frame: 0, sourceFrame: 0 },
        { frame: 60, sourceFrame: 120 },
      ],
    });
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, { type: 'item.setSpeed', itemId: item.id, speed: 2 });
    const t = d2.items[0]!;
    expect(t.speed).toBe(2);
    expect(t.timeRemap).toHaveLength(0);
    expect(t.durationFrames).toBe(60);
    expect(sourceOutFrame(t)).toBe(120);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('setKeyframes replaces one property and inverts', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 60);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.setKeyframes',
      itemId: item.id,
      property: 'transform.opacity',
      keyframes: [
        { frame: 0, value: 0, easing: 'linear' },
        { frame: 60, value: 1, easing: 'linear' },
      ],
    });
    expect(d2.items[0]!.keyframes['transform.opacity']).toHaveLength(2);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]!.keyframes['transform.opacity']).toBeUndefined();
  });
});

describe('clone', () => {
  it('clones after the original and inverts', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 60);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const cloneId = newId('itm');
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.clone',
      itemId: item.id,
      newItemId: cloneId,
    });
    const c = d2.items.find((i) => i.id === cloneId)!;
    expect(c.startFrame).toBe(60);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items).toEqual(d1.items);
  });
});
