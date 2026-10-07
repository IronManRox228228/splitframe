import { describe, expect, it } from 'vitest';
import {
  HEADER_W,
  dragCommit,
  frameFromPointer,
  marqueeHits,
  rowIndexAtY,
  rulerInterval,
  scrollForZoom,
  trackLayout,
  trimHandleWidth,
  type DragState,
} from './timeline-math.ts';

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

describe('group move', () => {
  it('moves every grouped item by the same delta in one op list', () => {
    const c = dragCommit({ ...base, ghostStart: 25, groupIds: [base.itemId, 'b'], groupOrigStarts: { b: 40 } }, false);
    expect(c?.label).toBe('Move clips');
    expect(c?.ops).toEqual([
      { type: 'item.move', itemId: base.itemId, startFrame: 25 },
      { type: 'item.move', itemId: 'b', startFrame: 55 },
    ]);
  });
});

describe('rulerInterval', () => {
  it('picks sub-second steps when zoomed far in and whole seconds otherwise', () => {
    expect(rulerInterval(40, 30)).toBe(5);
    expect(rulerInterval(3, 30)).toBe(30);
    expect(rulerInterval(0.2, 30)).toBe(450);
  });
  it('never returns an interval that would crowd labels when a larger one exists', () => {
    for (const px of [0.2, 0.5, 1, 3, 10, 40]) expect(rulerInterval(px, 30) * px).toBeGreaterThanOrEqual(88);
  });
});

describe('layout helpers', () => {
  it('maps y offsets to rows, clamping at the ends', () => {
    const layout = trackLayout(['text', 'video', 'audio']);
    expect(rowIndexAtY(layout, -10)).toBe(0);
    expect(rowIndexAtY(layout, layout[1]!.top + 1)).toBe(1);
    expect(rowIndexAtY(layout, 9999)).toBe(2);
  });
  it('sizes trim handles', () => {
    expect(trimHandleWidth(10)).toBe(0);
    expect(trimHandleWidth(24)).toBe(8);
    expect(trimHandleWidth(300)).toBe(8);
  });
  it('keeps the anchor fixed when zooming', () => {
    expect(scrollForZoom(100, 300, 2)).toBe(HEADER_W + 200 - 300 < 0 ? 0 : HEADER_W + 200 - 300);
  });
  it('finds items inside a marquee', () => {
    const layout = trackLayout(['video', 'audio']);
    const items = [
      { id: 'a', rowIndex: 0, startFrame: 0, endFrame: 10 },
      { id: 'b', rowIndex: 1, startFrame: 50, endFrame: 60 },
    ];
    expect(marqueeHits(items, layout, { x1: 0, y1: 0, x2: 100, y2: 20 }, 3)).toEqual(['a']);
    expect(marqueeHits(items, layout, { x1: 0, y1: 0, x2: 400, y2: 200 }, 3)).toEqual(['a', 'b']);
  });
});
