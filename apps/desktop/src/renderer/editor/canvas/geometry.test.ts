import { describe, expect, it } from 'vitest';
import type { ItemBounds } from '@cutboard/renderer';
import type { Item } from '@cutboard/schema';
import { boxCorners, liveTransformOps, upsertKeyframe, rotateAboutCenter, scaleFromCorner, snapAxis } from './geometry.ts';

const b = (over: Partial<ItemBounds> = {}): ItemBounds => ({
  itemId: 'a', trackId: 't', type: 'shape', cx: 500, cy: 250, w: 200, h: 100, rotation: 0,
  anchorX: 500, anchorY: 250, baseX: 500, baseY: 250, sx: 1, sy: 1, animated: false, locked: false, ...over,
});

describe('snapAxis', () => {
  it('snaps to the nearest target inside the threshold only', () => {
    expect(snapAxis([498, 600], [500], 5)).toEqual({ delta: 2, target: 500 });
    expect(snapAxis([480], [500], 5)).toBeNull();
  });
});

describe('scaleFromCorner', () => {
  it('scales uniformly about the opposite corner', () => {
    const r = scaleFromCorner(b(), { scale: 1, scaleX: 1, scaleY: 1 }, 2, { x: 700, y: 350 }, false);
    // dragging br from (600,300) to (700,350) with tl (400,200) fixed => k = 1.5
    expect(r.scale).toBeCloseTo(1.5);
    expect(r.x).toBeCloseTo(400 + 1.5 * 100 - 500);
    expect(r.y).toBeCloseTo(200 + 1.5 * 50 - 250);
  });
  it('free scale changes the axes independently', () => {
    const r = scaleFromCorner(b(), { scale: 1, scaleX: 1, scaleY: 1 }, 2, { x: 800, y: 300 }, true);
    expect(r.scaleX).toBeCloseTo(2);
    expect(r.scaleY).toBeCloseTo(1);
  });
});

describe('rotateAboutCenter', () => {
  it('snaps and keeps a centered item in place', () => {
    const r = rotateAboutCenter(b(), 0, { x: 500, y: 100 }, { x: 640, y: 160 }, 15);
    expect(r.rotation! % 15).toBeCloseTo(0);
    expect(r.x).toBeCloseTo(0);
    expect(r.y).toBeCloseTo(0);
  });
  it('rotates an off-center anchor around the box center', () => {
    const r = rotateAboutCenter(b({ anchorX: 400 }), 0, { x: 600, y: 250 }, { x: 500, y: 350 }, null);
    expect(r.rotation).toBeCloseTo(90);
    expect(r.x! + 500).toBeCloseTo(500);
    expect(r.y! + 250).toBeCloseTo(150);
  });
});

it('boxCorners rotate', () => {
  const c = boxCorners(b({ rotation: 90 }));
  expect(c[0]!.x).toBeCloseTo(550);
  expect(c[0]!.y).toBeCloseTo(150);
});

describe('liveTransformOps', () => {
  const item = (keyframes: Item['keyframes']): Item =>
    ({
      id: 'a', startFrame: 10, keyframes,
      transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
    }) as unknown as Item;

  it('patches the static transform when nothing is keyframed', () => {
    expect(liveTransformOps(item({}), { x: 5.123, y: 0 }, 20)).toEqual([
      { type: 'item.update', itemId: 'a', patch: { transform: { x: 5.12 } } },
    ]);
  });

  it('adds a keyframe at the playhead for a keyframed x, in the same op list as static y', () => {
    const kfs = { 'transform.x': [{ frame: 0, value: 0, easing: 'linear' as const }, { frame: 30, value: 100, easing: 'linear' as const }] };
    const ops = liveTransformOps(item(kfs), { x: 40, y: 7 }, 20);
    expect(ops).toEqual([
      { type: 'item.update', itemId: 'a', patch: { transform: { y: 7 } } },
      {
        type: 'item.setKeyframes', itemId: 'a', property: 'transform.x',
        keyframes: [
          { frame: 0, value: 0, easing: 'linear' },
          { frame: 10, value: 40, easing: 'linear' },
          { frame: 30, value: 100, easing: 'linear' },
        ],
      },
    ]);
  });

  it('updates an existing keyframe at the playhead instead of duplicating it', () => {
    const kfs = [{ frame: 10, value: 1, easing: 'easeIn' as const }];
    expect(upsertKeyframe(kfs, 10, 9)).toEqual([{ frame: 10, value: 9, easing: 'easeIn' }]);
  });
});
