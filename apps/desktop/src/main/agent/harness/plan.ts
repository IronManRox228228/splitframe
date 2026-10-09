import { z } from 'zod';
import type { Snapshot } from './types.ts';
import { resolveAsset } from './handles.ts';

/**
 * EditPlan: what the planner (an LLM call constrained to this schema) produces for multi-step
 * jobs. Params are in seconds and names, never frames or ids. The harness orders the steps,
 * runs mechanical ones through the compiler (no LLM) and judgment ones through the executor,
 * and persists the plan on the project so follow-ups ("make it punchier") revise it.
 */

export const STEP_KINDS = [
  'assemble', // judgment: pick source ranges for a teaser / highlight / first cut
  'remove_pauses',
  'remove_fillers',
  'remove_retakes',
  'beat_cut',
  'captions',
  'music',
  'title',
  'canvas',
  'clip_edit', // anything else on clips: trim, move, delete, speed, volume (executor with clip tools)
  'export',
] as const;
export type StepKind = (typeof STEP_KINDS)[number];

/** Run order regardless of how the model listed them: cut first, decorate after, export last. */
const ORDER: Record<StepKind, number> = {
  clip_edit: 0,
  assemble: 1,
  remove_retakes: 2,
  remove_fillers: 3,
  remove_pauses: 4,
  beat_cut: 5,
  canvas: 6,
  captions: 7,
  music: 8,
  title: 9,
  export: 10,
};

export const stepParamsSchema = z.object({
  /** assemble: what the footage should be about ("the pricing section", "the product demo") */
  topic: z.string().max(200).optional(),
  /** assemble: wanted length in seconds */
  targetSec: z.number().min(1).max(3600).optional(),
  /** remove_pauses: pauses longer than this go */
  thresholdSec: z.number().min(0.1).max(5).optional(),
  /** remove_fillers: words to cut; default um, uh, er, ah, you know */
  words: z.array(z.string().max(30)).max(12).optional(),
  /** captions */
  preset: z.enum(['serif', 'bold', 'karaoke']).optional(),
  /** music: asset name, volume 0-1, duck under speech */
  asset: z.string().max(120).optional(),
  volume: z.number().min(0).max(1).optional(),
  duck: z.boolean().optional(),
  /** title */
  text: z.string().max(200).optional(),
  startSec: z.number().min(0).optional(),
  durationSec: z.number().min(0.5).max(600).optional(),
  /** canvas */
  aspect: z.enum(['16:9', '9:16', '1:1', '4:5', '4:3', '21:9']).optional(),
  /** export: "720p", "1080p", "TikTok / Reels / Shorts"... */
  exportPreset: z.string().max(60).optional(),
});
export type StepParams = z.infer<typeof stepParamsSchema>;

/** What the model writes. */
export const planDraftSchema = z.object({
  summary: z.string().max(300).describe('One sentence: what the finished video is'),
  steps: z
    .array(
      z.object({
        kind: z.enum(STEP_KINDS),
        goal: z.string().max(200).describe('What this step achieves, short'),
        params: stepParamsSchema,
        accept: z.array(z.string().max(160)).max(4).describe('How to tell the step worked'),
      }),
    )
    .min(1)
    .max(10),
});
export type PlanDraft = z.infer<typeof planDraftSchema>;

export type StepStatus = 'pending' | 'running' | 'done' | 'failed' | 'skipped';

export interface PlanStep {
  id: number;
  kind: StepKind;
  goal: string;
  params: StepParams;
  accept: string[];
  status: StepStatus;
  /** verified outcome or the failure, one line */
  note?: string;
  /** dry-run summary shown on the plan card before the step runs */
  preview?: string;
}

export interface EditPlan {
  request: string;
  summary: string;
  createdAt: string;
  steps: PlanStep[];
}

/** The plan step that carries out each routed intent (intents with no entry are handled by the general executor). */
const STEP_FOR_INTENT: Record<string, StepKind> = {
  captions: 'captions',
  canvas: 'canvas',
  pauses: 'remove_pauses',
  fillers: 'remove_fillers',
  retakes: 'remove_retakes',
  music: 'music',
  title: 'title',
  beats: 'beat_cut',
  export: 'export',
  assemble: 'assemble',
};

/** Step kinds the request calls for (by its routed intents) that the plan does not have. */
export function missingStepKinds(plan: Pick<EditPlan, 'steps'>, intents: string[]): StepKind[] {
  const have = new Set(plan.steps.map((s) => s.kind));
  const want = intents.map((i) => STEP_FOR_INTENT[i]).filter((k): k is StepKind => k !== undefined);
  return [...new Set(want)].filter((k) => !have.has(k));
}

/** JSON schema for grammar-constrained decoding of the draft. */
export function planJsonSchema(): Record<string, unknown> {
  const schema = z.toJSONSchema(planDraftSchema, { target: 'draft-7' }) as Record<string, unknown>;
  delete schema['$schema'];
  return schema;
}

export interface PlanValidation {
  plan: EditPlan | null;
  errors: string[];
}

/** Schema-check a draft, drop what cannot run, order the steps, and number them. */
export function validatePlan(raw: unknown, request: string, snap: Snapshot, now = new Date().toISOString()): PlanValidation {
  const parsed = planDraftSchema.safeParse(raw);
  if (!parsed.success) return { plan: null, errors: parsed.error.issues.map((i) => `${i.path.join('.') || 'plan'}: ${i.message}`) };
  const errors: string[] = [];
  const seen = new Set<string>();
  const kept: Omit<PlanStep, 'id'>[] = [];
  const hasSpeech = snap.transcripts.some((t) => t.words.length > 0);
  for (const step of parsed.data.steps) {
    const key = `${step.kind}:${step.params.text ?? ''}:${step.params.topic ?? ''}`;
    if (seen.has(key)) continue; // the model repeated a step
    seen.add(key);
    // the model must not invent what the user did not ask for
    if (step.kind === 'export' && !/\b(export|render)\b/i.test(request)) {
      errors.push('export step dropped: the request did not ask for an export');
      continue;
    }
    if (step.kind === 'remove_pauses' && step.params.thresholdSec !== undefined && !/(pauses?|silences?|gaps?)\D{0,30}(\d|half|quarter)|(\d|half|quarter)\D{0,30}(pauses?|silences?)/i.test(request)) delete step.params.thresholdSec;
    if (step.kind === 'title' && !step.params.text?.trim()) {
      errors.push('title step without text was dropped');
      continue;
    }
    if (step.kind === 'canvas' && !step.params.aspect) {
      errors.push('canvas step without an aspect was dropped');
      continue;
    }
    if (step.kind === 'music') {
      const a = resolveAsset(snap.assets, step.params.asset ?? '', 'audio');
      if (!a.ok) {
        errors.push(`music step dropped: ${a.error}`);
        continue;
      }
    }
    if ((step.kind === 'remove_pauses' || step.kind === 'remove_fillers' || step.kind === 'remove_retakes' || step.kind === 'captions') && !hasSpeech) {
      errors.push(`${step.kind} dropped: nothing on the project has a transcript`);
      continue;
    }
    kept.push({ kind: step.kind, goal: step.goal, params: step.params, accept: step.accept, status: 'pending' });
  }
  if (kept.length === 0) return { plan: null, errors: errors.length ? errors : ['the plan has no runnable steps'] };
  const ordered = kept.map((s, i) => ({ s, i })).sort((a, b) => ORDER[a.s.kind] - ORDER[b.s.kind] || a.i - b.i).map(({ s }) => s);
  return {
    plan: { request, summary: parsed.data.summary, createdAt: now, steps: ordered.map((s, i) => ({ ...s, id: i + 1 })) },
    errors,
  };
}

/** Stored plans come back from disk: re-validate the shape so a stale file cannot crash a turn. */
export function parseStoredPlan(raw: unknown): EditPlan | null {
  const schema = z.object({
    request: z.string(),
    summary: z.string(),
    createdAt: z.string(),
    steps: z.array(
      z.object({
        id: z.number(),
        kind: z.enum(STEP_KINDS),
        goal: z.string(),
        params: stepParamsSchema,
        accept: z.array(z.string()),
        status: z.enum(['pending', 'running', 'done', 'failed', 'skipped']),
        note: z.string().optional(),
        preview: z.string().optional(),
      }),
    ),
  });
  const r = schema.safeParse(raw);
  return r.success ? r.data : null;
}

const MARK: Record<StepStatus, string> = { pending: ' ', running: '>', done: 'x', failed: '!', skipped: '-' };

/** The plan as the state doc and the plan card show it. */
export function renderPlan(plan: EditPlan): string {
  const lines = [`Goal: ${plan.summary}`];
  for (const s of plan.steps) lines.push(`${s.id}. [${MARK[s.status]}] ${s.kind}: ${s.goal}${s.note ? ` (${s.note})` : ''}`);
  return lines.join('\n');
}
