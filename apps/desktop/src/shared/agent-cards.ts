/**
 * What the main-process harness and the chat UI share about the agent's cards: the plan card
 * (steps with status and a dry-run summary, Run / Edit / Cancel) and the confirmation card
 * (Apply / Skip). Plain data over the event bus; the renderer never trusts it for anything but display.
 */

export const AGENT_MODE_IDS = ['plan', 'ask', 'default', 'auto'] as const;
export type AgentModeId = (typeof AGENT_MODE_IDS)[number];

export type PlanStepStatus = 'pending' | 'running' | 'done' | 'failed' | 'skipped';

export interface PlanCardStep {
  id: number;
  kind: string;
  goal: string;
  status: PlanStepStatus;
  /** verified outcome or the failure, one line */
  note?: string;
  /** dry-run summary: what the step will do, before it runs */
  preview?: string;
}

/**
 * proposed: waiting for Run / Edit / Cancel. launched: Run was pressed (the work streams in a new
 * turn). running / done: the plan executes in this turn. cancelled: dropped.
 */
export type PlanCardState = 'proposed' | 'launched' | 'running' | 'done' | 'cancelled';

export interface PlanCardData {
  planId: string;
  summary: string;
  state: PlanCardState;
  steps: PlanCardStep[];
}

export type PlanAction = 'run' | 'cancel' | 'start' | 'finish';

/** Card state machine: only a proposed plan can be run or cancelled; a running one finishes. */
export function transitionPlan(state: PlanCardState, action: PlanAction): PlanCardState {
  switch (action) {
    case 'run':
      return state === 'proposed' ? 'launched' : state;
    case 'cancel':
      return state === 'proposed' ? 'cancelled' : state;
    case 'start':
      return state === 'proposed' || state === 'launched' ? 'running' : state;
    case 'finish':
      return state === 'running' ? 'done' : state;
  }
}

/** Steps at a glance: "2 of 3 done". */
export function planProgress(card: Pick<PlanCardData, 'steps'>): { done: number; total: number; failed: number } {
  return { done: card.steps.filter((s) => s.status === 'done').length, total: card.steps.length, failed: card.steps.filter((s) => s.status === 'failed').length };
}

export type ConfirmStatus = 'pending' | 'applied' | 'skipped';

export interface ConfirmCardData {
  id: string;
  tool: string;
  /** one line: what the change does and how the length moves, e.g. "Cut 3 s of pauses · 1:32 → 1:14" */
  line: string;
  /** clips added / removed / trimmed ... */
  counts: string;
  /** why this asks even in Auto / Default */
  reasons: string[];
  /** clips on the timeline the change touches (highlighted while the card is pending) */
  affectedIds: string[];
  status: ConfirmStatus;
}

export function resolveConfirm(card: ConfirmCardData, decision: 'apply' | 'skip'): ConfirmCardData {
  return card.status === 'pending' ? { ...card, status: decision === 'apply' ? 'applied' : 'skipped' } : card;
}

/** A plan update replaces the card with the same planId; a new plan appends. */
export function upsertPlanCard<T extends { planId: string }>(cards: T[], next: T): T[] {
  const i = cards.findIndex((c) => c.planId === next.planId);
  if (i === -1) return [...cards, next];
  const out = [...cards];
  out[i] = next;
  return out;
}
