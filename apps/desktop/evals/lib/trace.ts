import type { HistoryTurn, StepTrace, ToolCallTrace, TurnTrace } from '../types.ts';

/** Tool results are clipped in the trace so one getTimeline dump cannot bloat the results file. */
const MAX_RESULT_CHARS = 4000;

export function clipValue(value: unknown, max = MAX_RESULT_CHARS): unknown {
  if (value === undefined) return undefined;
  let json: string;
  try {
    json = JSON.stringify(value);
  } catch {
    return String(value);
  }
  if (json.length <= max) return value;
  return { clipped: true, chars: json.length, preview: json.slice(0, max) };
}

const errorMessage = (err: unknown): string => {
  if (err instanceof Error) return err.message;
  if (err && typeof err === 'object' && 'message' in err) return String((err as { message: unknown }).message);
  return typeof err === 'string' ? err : JSON.stringify(err);
};

/**
 * Collects one chat turn as a trace. Plugs into the app's ChatSink: `emit` receives the same
 * events the chat UI gets (deltas, tool call/result, done), `onStep` receives the AI SDK's
 * finished steps, which is the only place tool calls that never ran (unknown tool, arguments
 * failing the schema) show up.
 */
export class TraceRecorder {
  private readonly started = Date.now();
  private text = '';
  private deltas: TurnTrace['deltas'] = [];
  private calls: ToolCallTrace[] = [];
  private open = new Map<string, { call: ToolCallTrace; startedAt: number }>();
  private steps: StepTrace[] = [];
  private finishReason: string | undefined;
  private error: string | undefined;
  private note: string | undefined;
  private confirmations: { line: string; reasons: string[] }[] = [];

  constructor(private readonly message: string) {}

  emit = (type: string, payload: Record<string, unknown>): void => {
    if (type === 'chat:delta') {
      const text = String(payload['text'] ?? '');
      this.text += text;
      this.deltas.push({ tMs: Date.now() - this.started, text });
    } else if (type === 'chat:tool') {
      const callId = payload['callId'] === undefined ? undefined : String(payload['callId']);
      const tool = String(payload['tool']);
      if (payload['phase'] === 'call') {
        const call: ToolCallTrace = { step: this.steps.length, ...(callId ? { callId } : {}), tool, args: payload['args'], durationMs: 0 };
        this.calls.push(call);
        this.open.set(callId ?? `${tool}#${this.calls.length}`, { call, startedAt: Date.now() });
      } else {
        const key = callId && this.open.has(callId) ? callId : [...this.open.keys()].reverse().find((k) => this.open.get(k)!.call.tool === tool);
        const entry = key ? this.open.get(key) : undefined;
        if (entry) {
          entry.call.durationMs = Date.now() - entry.startedAt;
          if (payload['error'] !== undefined) entry.call.error = String(payload['error']);
          else entry.call.result = clipValue(payload['result']);
          this.open.delete(key!);
        }
      }
    } else if (type === 'chat:confirm') {
      if (payload['phase'] === 'ask') this.confirmations.push({ line: String(payload['line'] ?? ''), reasons: (payload['reasons'] as string[] | undefined) ?? [] });
    } else if (type === 'chat:done') {
      if (payload['finishReason'] !== undefined) this.finishReason = String(payload['finishReason']);
      if (payload['error'] !== undefined) this.error = String(payload['error']);
      if (payload['note'] !== undefined) this.note = String(payload['note']);
    }
  };

  /** AI SDK step result: kept loosely typed, only the fields the trace needs are read. */
  onStep = (raw: unknown): void => {
    const step = raw as {
      finishReason?: string;
      text?: string;
      toolCalls?: { toolName: string; input?: unknown; invalid?: boolean; error?: unknown }[];
      usage?: { inputTokens?: number; outputTokens?: number };
    };
    const index = this.steps.length;
    const toolCalls = (step.toolCalls ?? []).map((c) => {
      const invalid = Boolean(c.invalid);
      if (invalid) {
        this.calls.push({ step: index, tool: c.toolName, args: c.input, invalid: true, error: errorMessage(c.error ?? 'invalid tool call'), durationMs: 0 });
      }
      return { tool: c.toolName, invalid, ...(invalid ? { error: errorMessage(c.error ?? 'invalid tool call') } : {}) };
    });
    this.steps.push({
      index,
      finishReason: step.finishReason ?? 'unknown',
      text: step.text ?? '',
      toolCalls,
      ...(step.usage?.inputTokens !== undefined ? { inputTokens: step.usage.inputTokens } : {}),
      ...(step.usage?.outputTokens !== undefined ? { outputTokens: step.usage.outputTokens } : {}),
    });
  };

  finish(extra: { error?: string; timedOut?: boolean } = {}): TurnTrace {
    const error = extra.error ?? this.error;
    return {
      message: this.message,
      text: this.text,
      deltas: this.deltas,
      toolCalls: this.calls,
      steps: this.steps,
      stepCount: this.steps.length,
      capHit: this.finishReason === 'tool-calls',
      ...(this.finishReason ? { finishReason: this.finishReason } : {}),
      wallMs: Date.now() - this.started,
      ...(error ? { error } : {}),
      ...(this.note ? { note: this.note } : {}),
      ...(this.confirmations.length > 0 ? { confirmations: this.confirmations } : {}),
      ...(extra.timedOut ? { timedOut: true } : {}),
    };
  }
}

/** Earlier turns as the chat UI passes them: plain text, or a "[used tools: ...]" stub when the reply had no text. */
export function historyAfter(history: HistoryTurn[], trace: TurnTrace): HistoryTurn[] {
  const tools = [...new Set(trace.toolCalls.map((c) => c.tool))];
  const reply = trace.text || (tools.length > 0 ? `[used tools: ${tools.join(', ')}]` : '');
  return [...history, { role: 'user', content: trace.message }, ...(reply ? [{ role: 'assistant' as const, content: reply }] : [])];
}

export interface TraceStats {
  toolCalls: number;
  invalidCalls: number;
  failedCalls: number;
  steps: number;
  capHit: boolean;
  wallMs: number;
  errors: string[];
  /** tool name -> number of calls */
  byTool: Record<string, number>;
}

export function traceStats(turns: TurnTrace[]): TraceStats {
  const stats: TraceStats = { toolCalls: 0, invalidCalls: 0, failedCalls: 0, steps: 0, capHit: false, wallMs: 0, errors: [], byTool: {} };
  for (const t of turns) {
    stats.steps += t.stepCount;
    stats.wallMs += t.wallMs;
    stats.capHit ||= t.capHit;
    if (t.error) stats.errors.push(t.error);
    for (const c of t.toolCalls) {
      stats.toolCalls++;
      stats.byTool[c.tool] = (stats.byTool[c.tool] ?? 0) + 1;
      if (c.invalid) stats.invalidCalls++;
      else if (c.error) stats.failedCalls++;
    }
  }
  return stats;
}
