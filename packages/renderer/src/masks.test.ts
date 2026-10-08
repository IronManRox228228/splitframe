import { describe, expect, it } from 'vitest';
import { createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { Item } from '@cutboard/schema';
import { applyMasks } from './compositor.ts';

/** Minimal 2D context recording clip paths in device space (translate/scale only). */
function mockCtx() {
  let m = { a: 1, d: 1, e: 0, f: 0 };
  const path: { kind: string; box: number[] }[] = [];
  const clips: { path: typeof path; rule: string }[] = [];
  const ctx = {
    getTransform: () => ({ ...m }),
    setTransform: (t: typeof m) => { m = { ...t }; },
    translate: (x: number, y: number) => { m = { ...m, e: m.e + m.a * x, f: m.f + m.d * y }; },
    scale: (x: number, y: number) => { m = { ...m, a: m.a * x, d: m.d * y }; },
    rotate: () => {},
    beginPath: () => { path.length = 0; },
    rect: (x: number, y: number, w: number, h: number) =>
      path.push({ kind: 'rect', box: [m.e + m.a * x, m.f + m.d * y, m.a * w, m.d * h] }),
    ellipse: (x: number, y: number, rx: number, ry: number) =>
      path.push({ kind: 'ellipse', box: [m.e + m.a * x, m.f + m.d * y, m.a * rx, m.d * ry] }),
    moveTo: () => {}, lineTo: () => {}, closePath: () => {},
    clip: (rule = 'nonzero') => clips.push({ path: [...path], rule }),
  };
  return { ctx: ctx as never, clips };
}

const style = {
  fontFamily: 'Geist', fontSize: 20, fontWeight: 700, color: '#fff', strokeWidth: 0,
  align: 'center', lineHeight: 1.5, letterSpacing: 0, uppercase: false, padding: 0, borderRadius: 0,
};

function itemWithMask(shape: 'rect' | 'ellipse', invert: boolean): Item {
  const doc = createEmptyDoc({ id: 'p', name: 'p', width: 1000, height: 500 });
  const item = createItem('text', {
    id: 'a', trackId: doc.tracks[0]!.id, startFrame: 0, durationFrames: 30, props: { text: 'x', style },
  } as never) as Item;
  return { ...item, masks: [{ id: 'm', shape, feather: 0, invert }] } as Item;
}

const target = { cx: 600, cy: 200, sx: 2, sy: 2, rotation: 0, w: 100, h: 50 };

describe('applyMasks', () => {
  it('clips rect masks to the item box, not the canvas', () => {
    const { ctx, clips } = mockCtx();
    applyMasks(ctx, itemWithMask('rect', false), 1000, 500, target);
    expect(clips).toHaveLength(1);
    expect(clips[0]!.rule).toBe('nonzero');
    expect(clips[0]!.path).toEqual([{ kind: 'rect', box: [500, 150, 200, 100] }]);
  });

  it('clips ellipse masks to the item box', () => {
    const { ctx, clips } = mockCtx();
    applyMasks(ctx, itemWithMask('ellipse', false), 1000, 500, target);
    expect(clips[0]!.path).toEqual([{ kind: 'ellipse', box: [600, 200, 100, 50] }]);
  });

  it('inverts by adding the canvas rect with the even-odd rule', () => {
    const { ctx, clips } = mockCtx();
    applyMasks(ctx, itemWithMask('rect', true), 1000, 500, target);
    expect(clips[0]!.rule).toBe('evenodd');
    expect(clips[0]!.path).toEqual([
      { kind: 'rect', box: [500, 150, 200, 100] },
      { kind: 'rect', box: [0, 0, 1000, 500] },
    ]);
  });
});
