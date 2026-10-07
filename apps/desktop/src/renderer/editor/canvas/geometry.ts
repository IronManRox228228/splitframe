import type { ItemBounds } from '@cutboard/renderer';

/** Pure math for on-canvas editing (canvas pixel space). */

export interface Pt {
  x: number;
  y: number;
}

/** Transform fields a drag can change; only these are written back. */
export interface LiveTransform {
  x?: number;
  y?: number;
  scale?: number;
  scaleX?: number;
  scaleY?: number;
  rotation?: number;
}

export const rad = (deg: number): number => (deg * Math.PI) / 180;

export function rotatePoint(p: Pt, deg: number): Pt {
  const c = Math.cos(rad(deg));
  const s = Math.sin(rad(deg));
  return { x: p.x * c - p.y * s, y: p.x * s + p.y * c };
}

/** Corners tl, tr, br, bl of the rotated box. */
export function boxCorners(b: Pick<ItemBounds, 'cx' | 'cy' | 'w' | 'h' | 'rotation'>): Pt[] {
  const hw = b.w / 2;
  const hh = b.h / 2;
  return [
    { x: -hw, y: -hh },
    { x: hw, y: -hh },
    { x: hw, y: hh },
    { x: -hw, y: hh },
  ].map((p) => {
    const r = rotatePoint(p, b.rotation);
    return { x: b.cx + r.x, y: b.cy + r.y };
  });
}

/** Axis-aligned extent of the rotated box. */
export function boxExtent(b: Pick<ItemBounds, 'cx' | 'cy' | 'w' | 'h' | 'rotation'>): {
  left: number;
  right: number;
  top: number;
  bottom: number;
} {
  const cs = boxCorners(b);
  const xs = cs.map((p) => p.x);
  const ys = cs.map((p) => p.y);
  return { left: Math.min(...xs), right: Math.max(...xs), top: Math.min(...ys), bottom: Math.max(...ys) };
}

/**
 * Find the smallest shift that lines one of `candidates` up with one of `targets`, within
 * `threshold`. Returns the delta to add and the target that was hit (for drawing a guide).
 */
export function snapAxis(candidates: number[], targets: number[], threshold: number): { delta: number; target: number } | null {
  let best: { delta: number; target: number } | null = null;
  for (const c of candidates) {
    for (const t of targets) {
      const d = t - c;
      if (Math.abs(d) <= threshold && (!best || Math.abs(d) < Math.abs(best.delta))) best = { delta: d, target: t };
    }
  }
  return best;
}

/** Canvas-space targets that moving items snap to: edges and center lines. */
export function snapTargets(canvasW: number, canvasH: number): { xs: number[]; ys: number[] } {
  return { xs: [0, canvasW / 2, canvasW], ys: [0, canvasH / 2, canvasH] };
}

/**
 * Corner-drag scaling about the opposite corner. `corner` is the index (tl,tr,br,bl) being
 * dragged, `p` the pointer. Uniform unless `free`.
 */
export function scaleFromCorner(
  b: ItemBounds,
  baseTransform: { scale: number; scaleX: number; scaleY: number },
  corner: number,
  p: Pt,
  free: boolean,
): LiveTransform {
  const cs = boxCorners(b);
  const P0 = cs[corner]!;
  const O = cs[(corner + 2) % 4]!;
  const anchorRel = { x: b.anchorX - O.x, y: b.anchorY - O.y };
  const MIN = 0.02;
  if (!free) {
    const vx = P0.x - O.x;
    const vy = P0.y - O.y;
    const len2 = vx * vx + vy * vy || 1;
    const k = Math.max(MIN, ((p.x - O.x) * vx + (p.y - O.y) * vy) / len2);
    return {
      scale: baseTransform.scale * k,
      x: O.x + k * anchorRel.x - b.baseX,
      y: O.y + k * anchorRel.y - b.baseY,
    };
  }
  // free: scale each local axis independently
  const toLocal = (v: Pt) => rotatePoint(v, -b.rotation);
  const v0 = toLocal({ x: P0.x - O.x, y: P0.y - O.y });
  const v1 = toLocal({ x: p.x - O.x, y: p.y - O.y });
  const kx = Math.max(MIN, Math.abs(v0.x) < 1e-6 ? 1 : v1.x / v0.x);
  const ky = Math.max(MIN, Math.abs(v0.y) < 1e-6 ? 1 : v1.y / v0.y);
  const la = toLocal(anchorRel);
  const na = rotatePoint({ x: la.x * kx, y: la.y * ky }, b.rotation);
  return {
    scaleX: baseTransform.scaleX * kx,
    scaleY: baseTransform.scaleY * ky,
    x: O.x + na.x - b.baseX,
    y: O.y + na.y - b.baseY,
  };
}

/** Rotate about the box center by the pointer's angular travel; optional snap to `step` degrees. */
export function rotateAboutCenter(
  b: ItemBounds,
  startRotation: number,
  startPointer: Pt,
  p: Pt,
  step: number | null,
): LiveTransform {
  const a0 = Math.atan2(startPointer.y - b.cy, startPointer.x - b.cx);
  const a1 = Math.atan2(p.y - b.cy, p.x - b.cx);
  let rotation = startRotation + ((a1 - a0) * 180) / Math.PI;
  if (step) rotation = Math.round(rotation / step) * step;
  const delta = rotation - startRotation;
  const rel = rotatePoint({ x: b.anchorX - b.cx, y: b.anchorY - b.cy }, delta);
  const norm = ((((rotation + 180) % 360) + 360) % 360) - 180;
  return {
    rotation: norm,
    x: b.cx + rel.x - b.baseX,
    y: b.cy + rel.y - b.baseY,
  };
}
