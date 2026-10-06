import { describe, expect, it } from 'vitest';
import { HEADER_W, dragCommit, frameFromPointer, type DragState } from './timeline-math.ts';

const base: DragState = {
  kind: 'move',
  itemId: 'itm_0000000000a',
  pointerId: 1,
  grabOffsetFrames: 0,
  origStart: 10,
  origTrackId: 'trk_a',
  ghostStart: 10,
  ghostTrackId: 'trk_a',
};

describe('frameFromPointer', () => {
  it('maps the start of the lane (just after the header) to frame 0', () => {
    expect(frameFromPointer(100 + HEADER_W, 100, 3)).toBe(0);
  });

  it('removes the header width before dividing by the zoom', () => {
    expect(frameFromPointer(100 + HEADER_W + 90, 100, 3)).toBe(30);
  });

  it('clamps pointer positions over the header to frame 0', () => {
    expect(frameFromPointer(120, 100, 3)).toBe(0);
  });
});

describe('dragCommit', () => {
  it('commits a move once the ghost differs from the original position', () => {
    const c = dragCommit({ ...base, ghostStart: 42 }, false);
    expect(c?.ops).toEqual([{ type: 'item.move', itemId: base.itemId, startFrame: 42 }]);
  });

  it('includes the track when it changed', () => {
    const c = dragCommit({ ...base, ghostTrackId: 'trk_b' }, false);
    expect(c?.ops).toEqual([{ type: 'item.move', itemId: base.itemId, trackId: 'trk_b' }]);
  });

  it('commits nothing for an unmoved drag', () => {
    expect(dragCommit(base, false)).toBeNull();
  });

  it('commits trims with the ripple flag', () => {
    const c = dragCommit({ ...base, kind: 'trim-out', ghostFrame: 77 }, true);
    expect(c?.ops).toEqual([{ type: 'item.trim', itemId: base.itemId, edge: 'out', frame: 77, ripple: true }]);
    expect(dragCommit({ ...base, kind: 'trim-in' }, false)).toBeNull();
  });
});
