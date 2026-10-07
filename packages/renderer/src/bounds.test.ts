import { describe, expect, it } from 'vitest';
import { createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { Item, TimelineDoc } from '@cutboard/schema';
import { hitTestBounds, itemBoundsAt, pointInBounds } from './bounds.ts';

const measure = (text: string) => text.length * 10;
const style = {
  fontFamily: 'Geist', fontSize: 20, fontWeight: 700, color: '#fff', strokeWidth: 0,
  align: 'center', lineHeight: 1.5, letterSpacing: 0, uppercase: false, padding: 0, borderRadius: 0,
};

function docWith(items: Item[]): TimelineDoc {
  const doc = createEmptyDoc({ id: 'p', name: 'p', width: 1000, height: 500 });
  return { ...doc, items };
}
function text(id: string, over: Record<string, unknown> = {}, tstyle: Record<string, unknown> = {}): Item {
  return createItem('text', {
    id, trackId: 'p_text', startFrame: 0, durationFrames: 30,
    props: { text: 'hello', style: { ...style, ...tstyle } },
    ...over,
  } as never) as Item;
}

describe('itemBoundsAt', () => {
  it('centers a text box on the canvas and sizes it from the measurement', () => {
    const [b] = itemBoundsAt(docWith([text('a')]), 0, 1000, 500, { measure });
    expect(b).toMatchObject({ cx: 500, cy: 250, w: 50, h: 30, rotation: 0 });
  });

  it('applies translation, scale and rotation', () => {
    const it1 = text('a', { transform: { x: 100, y: -50, scale: 2, scaleX: 1, scaleY: 1, rotation: 90, opacity: 1 } });
    const [b] = itemBoundsAt(docWith([it1]), 0, 1000, 500, { measure });
    expect(b).toMatchObject({ cx: 600, cy: 200, w: 100, h: 60, rotation: 90 });
    // rotated 90deg: the 100x60 box is now 60 wide, 100 tall around its center
    expect(pointInBounds(b!, 600, 240)).toBe(true);
    expect(pointInBounds(b!, 640, 200)).toBe(false);
  });

  it('offsets left-aligned text from its anchor', () => {
    const [b] = itemBoundsAt(docWith([text('a', {}, { align: 'left' })]), 0, 1000, 500, { measure });
    expect(b!.cx).toBe(525);
    expect(b!.anchorX).toBe(500);
  });

  it('fits video into the canvas (contain)', () => {
    const v = createItem('video', { id: 'v', trackId: 'p_main', startFrame: 0, durationFrames: 30, assetId: 'a1', sourceInFrame: 0 } as never) as Item;
    const [b] = itemBoundsAt(docWith([v]), 0, 1000, 500, { mediaSize: () => ({ width: 1000, height: 1000 }) });
    expect(b).toMatchObject({ w: 500, h: 500, cx: 500, cy: 250 });
  });

  it('skips items outside their time range, on hidden tracks, and audio', () => {
    const doc = docWith([text('a')]);
    expect(itemBoundsAt(doc, 30, 1000, 500, { measure })).toHaveLength(0);
    const hidden = { ...doc, tracks: doc.tracks.map((t) => (t.id === 'p_text' ? { ...t, hidden: true } : t)) };
    expect(itemBoundsAt(hidden, 0, 1000, 500, { measure })).toHaveLength(0);
  });

  it('flags locked tracks', () => {
    const doc = docWith([text('a')]);
    const locked = { ...doc, tracks: doc.tracks.map((t) => (t.id === 'p_text' ? { ...t, locked: true } : t)) };
    expect(itemBoundsAt(locked, 0, 1000, 500, { measure })[0]!.locked).toBe(true);
  });

  it('places captions at placementY and only while a card shows', () => {
    const cap = createItem('caption', {
      id: 'c', trackId: 'p_text', startFrame: 0, durationFrames: 90,
      props: {
        words: [{ w: 'hi', startMs: 0, endMs: 500 }, { w: 'there', startMs: 500, endMs: 900 }],
        maxWordsPerCard: 4,
        style: { ...style, highlight: 'none', highlightColor: '#f00', placementY: 0.8, maxCharsPerLine: 28 },
      },
    } as never) as Item;
    const [b] = itemBoundsAt(docWith([cap]), 0, 1000, 500, { measure });
    expect(b!.cy).toBe(400);
    expect(b!.w).toBe(20 + 50 + 10); // "hi" + "there" + one space
  });
});

describe('hitTestBounds', () => {
  it('returns the topmost (last drawn) box under the point', () => {
    const a = text('a');
    const b = text('b');
    const bounds = itemBoundsAt(docWith([a, b]), 0, 1000, 500, { measure });
    expect(hitTestBounds(bounds, 500, 250)?.itemId).toBe(bounds[bounds.length - 1]!.itemId);
    expect(hitTestBounds(bounds, 10, 10)).toBeNull();
  });
});
