import type { Asset, Item, TimelineDoc, Transcript } from '@cutboard/schema';
import type { FixtureName, Manifest } from './lib/manifest.ts';

/** One scored assertion. `partial` (0..1) is the credit earned; `pass` means full credit. */
export interface Check {
  name: string;
  pass: boolean;
  detail: string;
  weight: number;
  partial: number;
  /** "didn't break anything" check: scales the score instead of adding to it (score.ts finish) */
  guard?: boolean;
}

export interface CheckResult {
  /** weighted mean of the checks' partial credit, 0..1 */
  score: number;
  checks: Check[];
}

// ---------------- traces ----------------

export interface ToolCallTrace {
  /** model step (0-based) the call belongs to */
  step: number;
  callId?: string;
  tool: string;
  args: unknown;
  /** tool output (large values are clipped) */
  result?: unknown;
  /** runtime error the tool threw (the model sees it as {error}) */
  error?: string;
  /** rejected before running: unknown tool name or arguments that fail the schema */
  invalid?: boolean;
  durationMs: number;
}

export interface StepTrace {
  index: number;
  finishReason: string;
  text: string;
  toolCalls: { tool: string; invalid: boolean; error?: string }[];
  inputTokens?: number;
  outputTokens?: number;
}

export interface TurnTrace {
  message: string;
  /** everything the assistant said, concatenated */
  text: string;
  deltas: { tMs: number; text: string }[];
  toolCalls: ToolCallTrace[];
  steps: StepTrace[];
  stepCount: number;
  /** the 12-step budget ran out while the model still wanted to call tools */
  capHit: boolean;
  finishReason?: string;
  wallMs: number;
  error?: string;
  note?: string;
  timedOut?: boolean;
}

export interface HistoryTurn {
  role: 'user' | 'assistant';
  content: string;
}

// ---------------- driver ----------------

export interface DriverContext {
  chatId: string;
  signal: AbortSignal;
  /** harness settings the driver may need (provider, model, url) */
  provider: string;
  /** the task being run (the oracle driver scripts its edit by task id) */
  taskId: string;
  fps: number;
  manifest: Manifest;
  /** fixture file name -> asset id */
  assetIds: Record<string, string>;
}

/**
 * The harness under test. The baseline driver wraps the app's real chat turn; a future
 * planner/executor harness implements the same interface and runs the same tasks.
 */
export interface AgentDriver {
  id: string;
  /** needs no LLM server: the runner skips its health wait */
  offline?: boolean;
  runTurn(message: string, history: HistoryTurn[], ctx: DriverContext): Promise<TurnTrace>;
}

// ---------------- tasks ----------------

export interface ExportInfo {
  id: string;
  status: string;
  outputPath: string | null;
  exists: boolean;
  sizeBytes: number;
  width?: number;
  height?: number;
  error?: string | null;
}

export interface SetupContext {
  manifest: Manifest;
  fps: number;
  assets: Record<string, Asset>;
  /** put an imported fixture on the timeline (appended after the track's last item unless startSec is given) */
  addToTimeline(fixture: FixtureName, opts?: { startSec?: number }): Promise<string>;
  /** the editor's selection / playhead, as the renderer would report them */
  select(itemIds: string[]): void;
  setPlayheadSec(sec: number): void;
  doc(): TimelineDoc;
}

export interface CheckInput {
  manifest: Manifest;
  fps: number;
  /** fixture file name -> asset id */
  assetIds: Record<string, string>;
  initialDoc: TimelineDoc;
  /** the timeline after each user message */
  docs: TimelineDoc[];
  doc: TimelineDoc;
  assets: Asset[];
  transcripts: Transcript[];
  turns: TurnTrace[];
  exports: ExportInfo[];
  /** items placed by setup, by fixture name */
  setupItems: Record<string, string>;
}

export interface Task {
  id: string;
  title: string;
  difficulty: 'easy' | 'medium' | 'hard';
  /** fixtures imported through the asset pipeline */
  fixtures: FixtureName[];
  setup?(ctx: SetupContext): Promise<void>;
  /** user messages sent in order; later ones see the earlier turns as history */
  messages: string[];
  check(input: CheckInput): CheckResult;
  /** per-task wall-clock budget; default 10 minutes */
  timeoutMs?: number;
}

export type { Asset, Item, TimelineDoc, Transcript };
