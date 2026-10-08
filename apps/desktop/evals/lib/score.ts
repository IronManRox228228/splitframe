import type { Check, CheckResult } from '../types.ts';

/**
 * Build one check. `result` is either a boolean (all or nothing) or a number 0..1 (partial
 * credit; `pass` only at full credit).
 */
export function check(name: string, result: boolean | number, detail: string, weight = 1): Check {
  const partial = typeof result === 'boolean' ? (result ? 1 : 0) : Math.max(0, Math.min(1, Number.isFinite(result) ? result : 0));
  return { name, pass: partial >= 0.999, detail, weight, partial };
}

/**
 * A check that the edit did not break something (speech kept, other clips intact, no gaps).
 * An untouched timeline passes these, so they never earn credit on their own: see finish().
 */
export function guard(name: string, result: boolean | number, detail: string, weight = 1): Check {
  return { ...check(name, result, detail, weight), guard: true };
}

const weightedMean = (checks: Check[]): number => {
  const total = checks.reduce((a, c) => a + c.weight, 0);
  return total > 0 ? checks.reduce((a, c) => a + c.weight * c.partial, 0) / total : 1;
};

/**
 * Score = weighted mean of the goal checks x weighted mean of the guards. Doing nothing
 * scores 0 (guards alone earn nothing), and a goal reached by breaking something else
 * loses credit in proportion. A task with no goal checks scores 0.
 */
export function finish(checks: Check[]): CheckResult {
  const goals = checks.filter((c) => !c.guard);
  if (goals.length === 0) return { score: 0, checks };
  const score = weightedMean(goals) * weightedMean(checks.filter((c) => c.guard));
  return { score: Math.round(score * 1000) / 1000, checks };
}

/** Credit that falls off linearly: 1 inside `tolerance` of `target`, 0 at `zeroAt` or further. */
export function closeness(value: number, target: number, tolerance: number, zeroAt: number): number {
  const d = Math.abs(value - target);
  if (d <= tolerance) return 1;
  if (d >= zeroAt) return 0;
  return 1 - (d - tolerance) / (zeroAt - tolerance);
}

/** Credit for reaching `goal` from `floor` (linear), e.g. "fraction of the pauses removed". */
export function ramp(value: number, floor: number, goal: number): number {
  if (goal === floor) return value >= goal ? 1 : 0;
  return Math.max(0, Math.min(1, (value - floor) / (goal - floor)));
}

export const fmt = (n: number, digits = 2): string => (Math.round(n * 10 ** digits) / 10 ** digits).toString();

/** A check that could not even be evaluated (e.g. the thing it inspects is missing). */
export const missing = (name: string, detail: string, weight = 1): Check => check(name, false, detail, weight);
