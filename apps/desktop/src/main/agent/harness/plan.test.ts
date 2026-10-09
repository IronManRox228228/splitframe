import { describe, expect, it } from 'vitest';
import { missingStepKinds, parseStoredPlan, planJsonSchema, renderPlan, validatePlan } from './plan.ts';
import { fixtureSnapshot, speechTranscript } from './test-fixtures.ts';

const snap = () => fixtureSnapshot({ transcripts: [speechTranscript('ast_int', ['Hello there my friends today.'])] });
const step = (kind: string, params: Record<string, unknown> = {}) => ({ kind, goal: `do ${kind}`, params, accept: [] });

describe('EditPlan', () => {
  it('orders steps cut-first, decorate-after, export-last and numbers them', () => {
    const v = validatePlan(
      { summary: 'Ready to post', steps: [step('title', { text: 'Launch Day' }), step('export'), step('captions'), step('remove_pauses'), step('music', { asset: 'music.mp3' })] },
      'do it and export',
      snap(),
      'now',
    );
    expect(v.plan!.steps.map((s) => s.kind)).toEqual(['remove_pauses', 'captions', 'music', 'title', 'export']);
    expect(v.plan!.steps.map((s) => s.id)).toEqual([1, 2, 3, 4, 5]);
    expect(v.plan!.steps.every((s) => s.status === 'pending')).toBe(true);
  });

  it('drops steps that cannot run and repeated steps, reporting why', () => {
    const noSpeech = fixtureSnapshot();
    const v = validatePlan(
      { summary: 's', steps: [step('title', {}), step('music', { asset: 'nothing.mp3' }), step('canvas', {}), step('captions'), step('export'), step('export')] },
      'please export',
      noSpeech,
    );
    expect(v.plan!.steps.map((s) => s.kind)).toEqual(['export']);
    expect(v.errors.length).toBe(4);
  });

  it('drops what the user did not ask for: an export, an invented pause threshold', () => {
    const draft = { summary: 's', steps: [step('remove_pauses', { thresholdSec: 1.5 }), step('export')] };
    const v = validatePlan(draft, 'cut the long pauses', snap());
    expect(v.plan!.steps).toHaveLength(1);
    expect(v.plan!.steps[0]!.params.thresholdSec).toBeUndefined();
    const kept = validatePlan({ summary: 's', steps: [step('remove_pauses', { thresholdSec: 0.8 })] }, 'cut pauses over 0.8 seconds', snap());
    expect(kept.plan!.steps[0]!.params.thresholdSec).toBe(0.8);
  });

  it('rejects malformed drafts with readable errors', () => {
    const v = validatePlan({ summary: 's', steps: [{ kind: 'dance', goal: 'x', params: {}, accept: [] }] }, 'req', snap());
    expect(v.plan).toBeNull();
    expect(v.errors[0]).toContain('steps.0.kind');
    expect(validatePlan({ summary: 's', steps: [] }, 'req', snap()).plan).toBeNull();
  });

  it('exposes a JSON schema for constrained decoding and round-trips a stored plan', () => {
    const schema = planJsonSchema() as { properties: Record<string, unknown> };
    expect(Object.keys(schema.properties)).toEqual(['summary', 'steps']);
    const v = validatePlan({ summary: 'S', steps: [step('remove_pauses', { thresholdSec: 0.4 })] }, 'req', snap(), 'now');
    const back = parseStoredPlan(JSON.parse(JSON.stringify(v.plan)));
    expect(back).toEqual(v.plan);
    expect(parseStoredPlan({ junk: 1 })).toBeNull();
    expect(renderPlan(back!)).toContain('1. [ ] remove_pauses: do remove_pauses');
  });
});

describe('plan coverage', () => {
  const plan = (...kinds: string[]) => ({ steps: kinds.map((kind, i) => ({ id: i + 1, kind, goal: '', params: {}, accept: [], status: 'pending' })) }) as never;

  it('names the steps the routed intents call for that the plan lacks', () => {
    expect(missingStepKinds(plan('captions'), ['canvas', 'captions'])).toEqual(['canvas']);
    expect(missingStepKinds(plan('captions', 'canvas'), ['canvas', 'captions'])).toEqual([]);
    expect(missingStepKinds(plan('clip_edit'), ['pauses', 'fillers'])).toEqual(['remove_pauses', 'remove_fillers']);
  });

  it('clip and question intents need no particular step', () => {
    expect(missingStepKinds(plan('title'), ['clips', 'title'])).toEqual([]);
  });
});
