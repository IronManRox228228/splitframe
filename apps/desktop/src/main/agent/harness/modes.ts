import type { DocDiff } from './preview.ts';

/**
 * Agent modes: what the harness may do without asking. Pure decisions, no I/O, so every rule is
 * unit-tested. The user picks the mode per project; the agent has no tool that changes it.
 */

export const AGENT_MODES = ['plan', 'ask', 'default', 'auto'] as const;
export type AgentMode = (typeof AGENT_MODES)[number];
export const DEFAULT_MODE: AgentMode = 'default';

export const MODE_LABELS: Record<AgentMode, string> = { plan: 'Plan', ask: 'Ask before edits', default: 'Default', auto: 'Auto' };
export const MODE_HINTS: Record<AgentMode, string> = {
  plan: 'Reads and plans only. Nothing changes until you run the plan.',
  ask: 'Shows what each change will do and waits for Apply or Skip.',
  default: 'Small requests apply right away. Big jobs show a plan and wait for Run.',
  auto: 'Runs the whole job without stopping. Still verifies its work.',
};

export const isAgentMode = (v: unknown): v is AgentMode => typeof v === 'string' && (AGENT_MODES as readonly string[]).includes(v);

/** Shift+Tab order. */
export function nextMode(mode: AgentMode): AgentMode {
  return AGENT_MODES[(AGENT_MODES.indexOf(mode) + 1) % AGENT_MODES.length]!;
}

/** Why a step needs the user's say-so even in Auto. */
export type AskReason = 'media-removal' | 'overwrite-export' | 'cost' | 'destructive';

/** A change that drops more than this share of the timeline, or of its clips, is "destructive". */
export const DESTRUCTIVE_FRACTION = 0.5;
/** ...but only when at least this much is lost (trimming a 3 s clip to 1 s is not a disaster). */
export const DESTRUCTIVE_MIN_SEC = 2;
/** In Default mode a single-step request touching this many clips asks first, like a big job. */
export const MANY_CLIPS = 10;

export interface StepFacts {
  /** the step changes the project or the outside world (an export is an action, not a timeline edit) */
  mutates: boolean;
  /** undo / redo: the way back is never gated, except by Plan mode */
  history?: boolean;
  /** effect on the timeline, from a dry run (null: not computable, e.g. an export) */
  diff?: DocDiff | null;
  removesMedia?: boolean;
  overwritesExport?: boolean;
  /** money the step would spend (paid generation); nothing yet, the hook is here */
  costUsd?: number;
  /** the step belongs to a plan the user approved with Run: a big cut there was already seen and agreed to */
  approvedPlan?: boolean;
}

export function destructive(diff: DocDiff | null | undefined): boolean {
  if (!diff) return false;
  const lostSec = diff.beforeSec - diff.afterSec;
  if (diff.beforeSec > 0 && lostSec >= DESTRUCTIVE_MIN_SEC && lostSec / diff.beforeSec > DESTRUCTIVE_FRACTION) return true;
  return diff.clipsBefore >= 2 && diff.clipsGone / diff.clipsBefore > DESTRUCTIVE_FRACTION;
}

/** What always asks, whatever the mode. */
export function askReasons(f: StepFacts): AskReason[] {
  const out: AskReason[] = [];
  if (f.removesMedia) out.push('media-removal');
  if (f.overwritesExport) out.push('overwrite-export');
  if ((f.costUsd ?? 0) > 0) out.push('cost');
  if (!f.approvedPlan && destructive(f.diff)) out.push('destructive');
  return out;
}

export type StepAction = 'apply' | 'confirm' | 'block';

export interface StepDecision {
  action: StepAction;
  reasons: AskReason[];
  /** Default mode only: a single step that touches many clips asks too */
  many?: boolean;
}

export function decideStep(mode: AgentMode, f: StepFacts): StepDecision {
  if (!f.mutates) return { action: 'apply', reasons: [] };
  if (mode === 'plan') return { action: 'block', reasons: [] };
  const reasons = f.history ? [] : askReasons(f);
  if (reasons.length > 0) return { action: 'confirm', reasons };
  if (f.history) return { action: 'apply', reasons };
  if (mode === 'ask') return { action: 'confirm', reasons };
  if (mode === 'default' && f.diff && f.diff.clipsTouched >= MANY_CLIPS) return { action: 'confirm', reasons, many: true };
  return { action: 'apply', reasons };
}

export type PlanAction = 'run' | 'wait';

/**
 * After the planner wrote a plan: run it, or show the plan card and wait for Run?
 * Plan mode always waits. Default waits for big jobs (more than one step, or building a new cut).
 * Ask mode runs it but confirms every step; Auto just runs it.
 */
export function decidePlan(mode: AgentMode, plan: { steps: { kind: string }[] }): PlanAction {
  if (mode === 'plan') return 'wait';
  if (mode === 'default') return plan.steps.length >= 2 || plan.steps.some((s) => s.kind === 'assemble') ? 'wait' : 'run';
  return 'run';
}

/** Does this request go through the planner at all? Plan mode plans every change; the others only multi-step jobs. */
export function wantsPlan(mode: AgentMode, routeNeedsPlan: boolean, pureHistoryOrExport: boolean): boolean {
  if (mode === 'plan') return !pureHistoryOrExport;
  return routeNeedsPlan;
}

/** The reasons, in words for a confirmation card. */
export function reasonText(r: AskReason): string {
  switch (r) {
    case 'media-removal':
      return 'This removes media from the project.';
    case 'overwrite-export':
      return 'This overwrites an existing export file.';
    case 'cost':
      return 'This costs money.';
    case 'destructive':
      return 'This removes most of the timeline.';
  }
}
