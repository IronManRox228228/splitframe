import { Effect } from '@cutboard/schema';

/**
 * Effect registry: each type declares a human label, the numeric params it reads
 * (with ranges for UI/agent docs), and how it maps onto a canvas ctx.filter string.
 * Effects that cannot be expressed as a filter (e.g. distort) will get draw hooks later.
 */
export interface EffectDef {
  type: string;
  label: string;
  params: Record<string, { min: number; max: number; default: number; step?: number }>;
  toFilterPart(params: Record<string, number | string | boolean>): string | null;
}

const num = (params: Record<string, number | string | boolean>, key: string, fallback: number): number => {
  const v = params[key];
  return typeof v === 'number' && Number.isFinite(v) ? v : fallback;
};

export const EFFECTS: EffectDef[] = [
  {
    type: 'brightness',
    label: 'Brightness',
    params: { amount: { min: -1, max: 1, default: 0, step: 0.01 } },
    toFilterPart: (p) => `brightness(${Math.max(0, 1 + num(p, 'amount', 0))})`,
  },
  {
    type: 'contrast',
    label: 'Contrast',
    params: { amount: { min: 0, max: 3, default: 1, step: 0.01 } },
    toFilterPart: (p) => `contrast(${num(p, 'amount', 1)})`,
  },
  {
    type: 'saturation',
    label: 'Saturation',
    params: { amount: { min: 0, max: 3, default: 1, step: 0.01 } },
    toFilterPart: (p) => `saturate(${num(p, 'amount', 1)})`,
  },
  {
    type: 'blur',
    label: 'Gaussian Blur',
    params: { radiusPx: { min: 0, max: 100, default: 0, step: 0.5 } },
    toFilterPart: (p) => (num(p, 'radiusPx', 0) > 0 ? `blur(${num(p, 'radiusPx', 0)}px)` : null),
  },
  {
    type: 'hueRotate',
    label: 'Hue Rotate',
    params: { degrees: { min: -180, max: 180, default: 0, step: 1 } },
    toFilterPart: (p) => (num(p, 'degrees', 0) !== 0 ? `hue-rotate(${num(p, 'degrees', 0)}deg)` : null),
  },
  {
    type: 'grayscale',
    label: 'Grayscale',
    params: { amount: { min: 0, max: 1, default: 0, step: 0.01 } },
    toFilterPart: (p) => (num(p, 'amount', 0) > 0 ? `grayscale(${num(p, 'amount', 0)})` : null),
  },
  {
    type: 'sepia',
    label: 'Sepia',
    params: { amount: { min: 0, max: 1, default: 0, step: 0.01 } },
    toFilterPart: (p) => (num(p, 'amount', 0) > 0 ? `sepia(${num(p, 'amount', 0)})` : null),
  },
];

export function listEffects(): EffectDef[] {
  return EFFECTS;
}

/** Build a single ctx.filter string for an item's effect stack. */
export function effectsToFilter(effects: Effect[]): string {
  const parts: string[] = [];
  for (const effect of effects) {
    const def = EFFECTS.find((d) => d.type === effect.type);
    if (!def) continue;
    const part = def.toFilterPart(effect.params as Record<string, number | string | boolean>);
    if (part) parts.push(part);
  }
  return parts.length > 0 ? parts.join(' ') : 'none';
}
