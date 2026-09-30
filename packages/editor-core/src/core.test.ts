import { describe, expect, it } from 'vitest';
import { applyOp, applyOps, makeDoc, trackOfKind, videoItem, textItem, audioItem } from './test-helpers.ts';
import { newId } from '@cutboard/schema';
import { OpError } from './timeline-doc.ts';
import { docDurationFrames, itemEnd, sourceFrameAt, itemsOnTrack } from './timeline-doc.ts';

describe('item.add / remove / update / move', () => {
  it('adds an item and removes it again', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90);
    const { doc: d1, inverse } = applyOp(doc, { type: 'item.add', item });
    expect(d1.items).toHaveLength(1);
    const { doc: d2 } = applyOp(d1, inverse[0]!);
    expect(d2.items).toHaveLength(0);
  });

  it('does not mutate the input doc', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90);
    applyOp(doc, { type: 'item.add', item });
    expect(doc.items).toHaveLength(0);
  });

  it('rejects duplicate item ids', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    expect(() => applyOp(d1, { type: 'item.add', item })).toThrow(OpError);
  });

  it('rejects items on incompatible tracks', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90, { trackId: trackOfKind(doc, 'audio').id });
    expect(() => applyOp(doc, { type: 'item.add', item })).toThrow(OpError);
  });

  it('updates a patch and produces an exact inverse', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.update',
      itemId: item.id,
      patch: { transform: { x: 100, opacity: 0.5 }, volume: 0.25 },
    });
    const updated = d2.items[0]!;
    expect(updated.transform.x).toBe(100);
    expect(updated.transform.y).toBe(0); // merge, not replace
    expect(updated.transform.opacity).toBe(0.5);
    expect(updated.volume).toBe(0.25);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('whole-replaces props and restores them', () => {
    const doc = makeDoc();
    const item = textItem(doc, 0, 60, 'Before');
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, {
      type: 'item.update',
      itemId: item.id,
      patch: { props: { text: 'After', style: item.props.style } },
    });
    expect((d2.items[0] as { props: { text: string } }).props.text).toBe('After');
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]).toEqual(item);
  });

  it('moves an item between frames and between tracks', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 90);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const { doc: d2, inverse } = applyOp(d1, { type: 'item.move', itemId: item.id, startFrame: 30 });
    expect(d2.items[0]!.startFrame).toBe(30);
    const { doc: d3 } = applyOps(d2, inverse);
    expect(d3.items[0]!.startFrame).toBe(0);

    // now add a second video track and move the item to it
    const newTrackId = newId('trk');
    const { doc: d4 } = applyOp(d3, {
      type: 'track.add',
      trackId: newTrackId,
      kind: 'video',
      name: 'B-roll',
      locked: false,
      muted: false,
      hidden: false,
    });
    const { doc: d5, inverse: moveBack } = applyOp(d4, {
      type: 'item.move',
      itemId: item.id,
      trackId: newTrackId,
    });
    expect(d5.items[0]!.trackId).toBe(newTrackId);
    const { doc: d6 } = applyOps(d5, moveBack);
    expect(d6.items[0]!.trackId).toBe(d1.tracks[1]!.id);
  });

  it('item.remove throws on unknown ids with a hint', () => {
    const doc = makeDoc();
    expect(() => applyOp(doc, { type: 'item.remove', itemIds: ['itm_nope'], ripple: false })).toThrow(
      /not found/i,
    );
  });
});

describe('tracks', () => {
  it('adds a track at an index and removes it with its items', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 30);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    const trackId = newId('trk');
    const { doc: d2, inverse: addInverse } = applyOp(d1, {
      type: 'track.add',
      trackId,
      kind: 'overlay',
      name: 'Overlay',
      locked: false,
      muted: false,
      hidden: false,
      index: 1,
    });
    expect(d2.tracks[1]!.id).toBe(trackId);
    const { doc: d3, inverse: moveInverse } = applyOp(d2, {
      type: 'item.update',
      itemId: item.id,
      patch: { trackId },
    });
    const { doc: d4, inverse: removeInverse } = applyOp(d3, { type: 'track.remove', trackId });
    expect(d4.tracks.map((t) => t.id)).toEqual(d1.tracks.map((t) => t.id));
    expect(d4.items.find((i) => i.id === item.id)).toBeUndefined(); // removed with the track
    // undo everything: track comes back with its item, then the item moves home
    const { doc: d5 } = applyOps(d4, removeInverse);
    const { doc: d6 } = applyOps(d5, moveInverse);
    const { doc: d7 } = applyOps(d6, addInverse);
    expect(d7.tracks.map((t) => t.id)).toEqual(d1.tracks.map((t) => t.id));
    expect(d7.items.find((i) => i.id === item.id)!.trackId).toBe(d1.tracks[1]!.id);
  });

  it('reorders tracks and restores the order', () => {
    const doc = makeDoc();
    const ids = doc.tracks.map((t) => t.id);
    const reversed = [...ids].reverse();
    const { doc: d1, inverse } = applyOp(doc, { type: 'track.reorder', trackIds: reversed });
    expect(d1.tracks.map((t) => t.id)).toEqual(reversed);
    const { doc: d2 } = applyOps(d1, inverse);
    expect(d2.tracks.map((t) => t.id)).toEqual(ids);
  });

  it('rejects reorder with missing ids', () => {
    const doc = makeDoc();
    expect(() =>
      applyOp(doc, { type: 'track.reorder', trackIds: doc.tracks.slice(1).map((t) => t.id) }),
    ).toThrow(OpError);
  });
});

describe('markers', () => {
  it('add/update/remove roundtrip', () => {
    const doc = makeDoc();
    const marker = { id: newId('mrk'), frame: 45, label: 'Hit', color: '#f00' };
    const { doc: d1, inverse } = applyOp(doc, { type: 'marker.add', marker });
    expect(d1.markers).toHaveLength(1);
    const { doc: d2, inverse: uInv } = applyOp(d1, {
      type: 'marker.update',
      markerId: marker.id,
      patch: { label: 'Drop' },
    });
    expect(d2.markers[0]!.label).toBe('Drop');
    const { doc: d3 } = applyOps(d2, uInv);
    expect(d3.markers[0]!.label).toBe('Hit');
    const { doc: d4 } = applyOps(d3, inverse);
    expect(d4.markers).toHaveLength(0);
  });
});

describe('batch ops', () => {
  it('applies atomically: failure leaves the doc untouched', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 30);
    const { doc: d1 } = applyOp(doc, { type: 'item.add', item });
    expect(() =>
      applyOp(d1, {
        type: 'batch',
        ops: [
          { type: 'marker.add', marker: { id: newId('mrk'), frame: 10, label: 'x' } },
          { type: 'item.update', itemId: 'itm_missing', patch: { volume: 0.5 } },
        ],
      }),
    ).toThrow();
    // d1 unchanged (apply never mutates input)
    expect(d1.markers).toHaveLength(0);
  });

  it('rejects nested batches', () => {
    const doc = makeDoc();
    expect(() =>
      applyOp(doc, { type: 'batch', ops: [{ type: 'batch', ops: [{ type: 'project.rename', name: 'x' }] }] }),
    ).toThrow(/batch/i);
  });
});

describe('timeline queries', () => {
  it('computes doc duration and sorted track items', () => {
    const doc = makeDoc();
    const a = videoItem(doc, 30, 60);
    const b = videoItem(doc, 0, 30);
    const { doc: d1 } = applyOps(doc, [
      { type: 'item.add', item: a },
      { type: 'item.add', item: b },
      { type: 'item.add', item: audioItem(doc, 0, 120) },
    ]);
    expect(docDurationFrames(d1)).toBe(120);
    const main = trackOfKind(d1, 'video');
    expect(itemsOnTrack(d1, main.id).map((i) => i.startFrame)).toEqual([0, 30]);
    const first = itemsOnTrack(d1, main.id)[0]!;
    expect(itemEnd(first)).toBe(30);
  });

  it('maps timeline frames to source frames with speed', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 10, 90, { speed: 2, sourceInFrame: 5 });
    expect(sourceFrameAt(item, 10)).toBe(5);
    expect(sourceFrameAt(item, 11)).toBe(7);
    expect(sourceFrameAt(item, 12)).toBe(9);
  });

  it('maps frames through a time remap', () => {
    const doc = makeDoc();
    const item = videoItem(doc, 0, 60, {
      timeRemap: [
        { frame: 0, sourceFrame: 0 },
        { frame: 30, sourceFrame: 30 },
        { frame: 60, sourceFrame: 30 }, // freeze at the end
      ],
    });
    expect(sourceFrameAt(item, 15)).toBe(15);
    expect(sourceFrameAt(item, 45)).toBe(30);
    expect(sourceFrameAt(item, 70)).toBe(30);
  });
});
