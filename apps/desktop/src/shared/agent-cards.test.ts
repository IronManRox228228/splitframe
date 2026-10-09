import { describe, expect, it } from 'vitest';
import { planProgress, resolveConfirm, transitionPlan, upsertPlanCard, type ConfirmCardData, type PlanCardData } from './agent-cards.ts';

const card = (o: Partial<PlanCardData> = {}): PlanCardData => ({
  planId: 'p1',
  summary: 's',
  state: 'proposed',
  steps: [
    { id: 1, kind: 'captions', goal: 'a', status: 'pending' },
    { id: 2, kind: 'title', goal: 'b', status: 'pending' },
  ],
  ...o,
});

describe('plan card states', () => {
  it('proposed -> launched on Run, -> cancelled on Cancel', () => {
    expect(transitionPlan('proposed', 'run')).toBe('launched');
    expect(transitionPlan('proposed', 'cancel')).toBe('cancelled');
  });

  it('only a proposed plan can be run or cancelled', () => {
    for (const s of ['launched', 'running', 'done', 'cancelled'] as const) {
      expect(transitionPlan(s, 'run')).toBe(s);
      expect(transitionPlan(s, 'cancel')).toBe(s);
    }
  });

  it('a started plan runs, then finishes', () => {
    expect(transitionPlan('proposed', 'start')).toBe('running');
    expect(transitionPlan('launched', 'start')).toBe('running');
    expect(transitionPlan('running', 'finish')).toBe('done');
    expect(transitionPlan('proposed', 'finish')).toBe('proposed');
    expect(transitionPlan('cancelled', 'start')).toBe('cancelled');
  });

  it('counts step progress', () => {
    const c = card({ steps: [{ id: 1, kind: 'a', goal: '', status: 'done' }, { id: 2, kind: 'b', goal: '', status: 'failed' }, { id: 3, kind: 'c', goal: '', status: 'pending' }] });
    expect(planProgress(c)).toEqual({ done: 1, total: 3, failed: 1 });
  });

  it('an update replaces the card with the same planId, a new plan appends', () => {
    const a = card();
    const b = card({ state: 'running' });
    expect(upsertPlanCard([a], b)).toEqual([b]);
    expect(upsertPlanCard([a], card({ planId: 'p2' }))).toHaveLength(2);
  });
});

describe('confirmation card', () => {
  const c: ConfirmCardData = { id: 'h1', tool: 'removePauses', line: 'x', counts: '', reasons: [], affectedIds: [], status: 'pending' };
  it('resolves once', () => {
    const applied = resolveConfirm(c, 'apply');
    expect(applied.status).toBe('applied');
    expect(resolveConfirm(applied, 'skip').status).toBe('applied');
    expect(resolveConfirm(c, 'skip').status).toBe('skipped');
  });
});
