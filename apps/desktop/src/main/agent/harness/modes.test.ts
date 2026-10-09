import { describe, expect, it } from 'vitest';
import { askReasons, decidePlan, decideStep, destructive, nextMode, wantsPlan, type StepFacts } from './modes.ts';
import type { DocDiff } from './preview.ts';

const diff = (o: Partial<DocDiff> = {}): DocDiff => ({
  beforeSec: 60,
  afterSec: 58,
  clipsBefore: 4,
  clipsGone: 0,
  added: 0,
  removed: 0,
  trimmed: 0,
  moved: 0,
  changed: 0,
  captionsAdded: 0,
  captionsRemoved: 0,
  clipsTouched: 0,
  affectedIds: [],
  ...o,
});
const edit = (o: Partial<StepFacts> = {}): StepFacts => ({ mutates: true, diff: diff(), ...o });

describe('mode decisions', () => {
  it('a plain edit: Plan blocks, Ask confirms, Default and Auto apply', () => {
    expect(decideStep('plan', edit()).action).toBe('block');
    expect(decideStep('ask', edit()).action).toBe('confirm');
    expect(decideStep('default', edit()).action).toBe('apply');
    expect(decideStep('auto', edit()).action).toBe('apply');
  });

  it('reads never ask or block, in any mode', () => {
    for (const m of ['plan', 'ask', 'default', 'auto'] as const) expect(decideStep(m, { mutates: false }).action).toBe('apply');
  });

  it('exporting to a new file is fine in Auto and Default; overwriting always asks', () => {
    expect(decideStep('auto', { mutates: true }).action).toBe('apply');
    expect(decideStep('default', { mutates: true }).action).toBe('apply');
    for (const m of ['ask', 'default', 'auto'] as const) {
      const d = decideStep(m, { mutates: true, overwritesExport: true });
      expect(d.action).toBe('confirm');
      expect(d.reasons).toEqual(['overwrite-export']);
    }
  });

  it('media removal and cost ask in every mode that can act', () => {
    for (const m of ['ask', 'default', 'auto'] as const) {
      expect(decideStep(m, { mutates: true, removesMedia: true }).reasons).toEqual(['media-removal']);
      expect(decideStep(m, { mutates: true, costUsd: 0.5 }).action).toBe('confirm');
    }
    expect(decideStep('auto', { mutates: true, costUsd: 0 }).action).toBe('apply');
  });

  it('undo and redo apply without asking (except in Plan mode)', () => {
    expect(decideStep('ask', { mutates: true, history: true }).action).toBe('apply');
    expect(decideStep('auto', { mutates: true, history: true }).action).toBe('apply');
    expect(decideStep('plan', { mutates: true, history: true }).action).toBe('block');
  });

  it('Default asks when one step touches many clips', () => {
    const d = decideStep('default', edit({ diff: diff({ clipsTouched: 12 }) }));
    expect(d).toMatchObject({ action: 'confirm', many: true, reasons: [] });
    expect(decideStep('auto', edit({ diff: diff({ clipsTouched: 12 }) })).action).toBe('apply');
  });
});

describe('destructive-edit guard', () => {
  it('more than half of the duration gone', () => {
    expect(destructive(diff({ beforeSec: 60, afterSec: 20 }))).toBe(true);
    expect(destructive(diff({ beforeSec: 60, afterSec: 31 }))).toBe(false);
    expect(destructive(diff({ beforeSec: 60, afterSec: 30 }))).toBe(false); // exactly half is not "more than"
  });

  it('most clips deleted', () => {
    expect(destructive(diff({ clipsBefore: 4, clipsGone: 3, beforeSec: 60, afterSec: 55 }))).toBe(true);
    expect(destructive(diff({ clipsBefore: 4, clipsGone: 2, beforeSec: 60, afterSec: 55 }))).toBe(false);
    expect(destructive(diff({ clipsBefore: 1, clipsGone: 1, beforeSec: 3, afterSec: 0 }))).toBe(true);
  });

  it('a tiny loss is not destructive even when it is most of a tiny timeline', () => {
    expect(destructive(diff({ beforeSec: 3, afterSec: 1.5, clipsBefore: 1, clipsGone: 0 }))).toBe(false);
  });

  it('asks in Auto and Default, and says why', () => {
    const f = edit({ diff: diff({ beforeSec: 136, afterSec: 30 }) });
    expect(askReasons(f)).toEqual(['destructive']);
    expect(decideStep('auto', f)).toMatchObject({ action: 'confirm', reasons: ['destructive'] });
    expect(decideStep('default', f).action).toBe('confirm');
  });

  it('no diff (an export) is never destructive', () => {
    expect(destructive(null)).toBe(false);
  });
});

describe('plans and mode order', () => {
  it('Plan mode always waits for Run; Default waits for big jobs only; Ask and Auto run', () => {
    const one = { steps: [{ kind: 'captions' }] };
    const two = { steps: [{ kind: 'captions' }, { kind: 'title' }] };
    const cut = { steps: [{ kind: 'assemble' }] };
    expect(decidePlan('plan', one)).toBe('wait');
    expect(decidePlan('default', one)).toBe('run');
    expect(decidePlan('default', two)).toBe('wait');
    expect(decidePlan('default', cut)).toBe('wait');
    expect(decidePlan('ask', two)).toBe('run');
    expect(decidePlan('auto', two)).toBe('run');
  });

  it('only Plan mode plans single-step requests; undo and export are not planned', () => {
    expect(wantsPlan('plan', false, false)).toBe(true);
    expect(wantsPlan('plan', false, true)).toBe(false);
    expect(wantsPlan('default', false, false)).toBe(false);
    expect(wantsPlan('auto', true, false)).toBe(true);
  });

  it('Shift+Tab cycles through the four modes and wraps', () => {
    expect(nextMode('plan')).toBe('ask');
    expect(nextMode('ask')).toBe('default');
    expect(nextMode('default')).toBe('auto');
    expect(nextMode('auto')).toBe('plan');
  });
});
