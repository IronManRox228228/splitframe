import { z } from 'zod';
import type { ChatSink } from '../chat.ts';
import { fitPicks, type SentencePick } from './compiler.ts';
import { FACADE, snappedSentences, speechAssetIds, type FacadeTool, type Outcome } from './facade.ts';
import { jsonSchemaOf, type Llm, type LlmUsage } from './llm.ts';
import { parseStoredPlan, planJsonSchema, renderPlan, validatePlan, type EditPlan, type PlanStep } from './plan.ts';
import { INTENTS, ROUTER_PROMPT, looksCompound, routeNoModel, toolsetFor, needsPlan, type Intent, type Route } from './router.ts';
import { renderStateDoc } from './state-doc.ts';
import type { Backend, Snapshot } from './types.ts';
import { fmtDur, fmtTime } from './units.ts';

/**
 * Harness v1: the LLM decides what, code does how.
 *
 *   message -> router (rules, else one constrained call) -> [planner for multi-step jobs]
 *     -> per step: compiler macro (no LLM) | judgment call (constrained picks) | executor (few tools)
 *     -> verifier after every tool -> reply composed from verified facts.
 *
 * All model calls are sequential and every one starts from a freshly rendered state document;
 * the conversation is only the last few turns as short text.
 */

export interface HarnessDeps {
  backend: Backend;
  planner: Llm;
  executor: Llm;
  sink: Pick<ChatSink, 'emit' | 'onStep'>;
  signal?: AbortSignal;
}

export interface HistoryTurn {
  role: 'user' | 'assistant';
  content: string;
}

export interface StepRecord {
  label: string;
  tool: string;
  ok: boolean;
  summary: string;
  mutated: boolean;
}

export interface HarnessResult {
  reply: string;
  steps: StepRecord[];
  asked: boolean;
}

/** Rounds of tool calling one step may use (the first call, then fixes after a failed verification). */
export const MAX_ROUNDS = 3;

const EXECUTOR_SYSTEM = `You are the hands of a video editor. You see the project as clips (handles like V1·2), file names and seconds. Carry out the REQUEST by calling the tools now.
Rules: never ask the user when a sensible default exists (a title starts at 0 s and lasts 3 s, pauses are those over 0.5 s, music is quiet and ducked, export is 1080p unless told otherwise). Use clip handles or file names exactly as listed. All times are seconds. If a tool reports a problem, fix the arguments and call it again. Do not explain first; call the tool.`;

const PLANNER_SYSTEM = `You plan video edits. Write a short EditPlan for the request: only steps the user asked for, each with its kind and parameters in seconds or names.
Kinds: assemble (build a new cut from footage: teaser/highlights, params topic and targetSec), remove_pauses, remove_fillers, remove_retakes, beat_cut, captions, music (params asset = the music file name, volume 0-1, duck), title (params text, startSec, durationSec), canvas (params aspect), clip_edit (anything else on clips), export (params exportPreset).
Leave a parameter out unless the user stated it (defaults are applied by code): no thresholdSec unless they gave a duration, no export step unless they asked to export. Do not invent steps.`;

const QUESTION_SYSTEM = `You answer questions about a video project from its state document, in one or two plain sentences. If the request is a vague wish to edit, ask ONE short question that names what you need.`;

const clip = (s: string, n: number): string => (s.length > n ? `${s.slice(0, n)}...` : s);

function recent(history: HistoryTurn[]): string {
  const last = history.slice(-4).filter((t) => t.content.trim());
  return last.length ? `RECENT CONVERSATION:\n${last.map((t) => `${t.role}: ${clip(t.content.replace(/\s+/g, ' '), 220)}`).join('\n')}\n\n` : '';
}

let callSeq = 0;
const nextId = (): string => `h${++callSeq}`;

class Run {
  records: StepRecord[] = [];
  asked = '';
  constructor(
    readonly d: HarnessDeps,
    readonly history: HistoryTurn[],
  ) {}

  get sink() {
    return this.d.sink;
  }

  async state(plan?: EditPlan | null, excerpts = true): Promise<{ snap: Snapshot; doc: string }> {
    const snap = await this.d.backend.snapshot();
    return { snap, doc: renderStateDoc({ snap, notes: (id) => this.d.backend.notes(id), plan, excerpts }) };
  }

  step(usage: LlmUsage, text: string, calls: { name: string; args: unknown; invalid?: string }[]): void {
    this.sink.onStep?.({
      finishReason: calls.length > 0 ? 'tool-calls' : 'stop',
      text,
      toolCalls: calls.map((c) => ({ toolName: c.name, input: c.args, ...(c.invalid ? { invalid: true, error: c.invalid } : {}) })),
      usage,
    });
  }

  /** run one facade tool, reporting it to the chat UI / trace as a tool row */
  async runTool(name: string, args: unknown, label: string): Promise<Outcome> {
    const tool = FACADE[name];
    const callId = nextId();
    this.sink.emit('chat:tool', { tool: name, callId, args, phase: 'call' });
    if (!tool) {
      const out: Outcome = { ok: false, summary: `Unknown tool ${name}.`, findings: [], mutated: false };
      this.sink.emit('chat:tool', { tool: name, callId, args, phase: 'result', error: out.summary });
      return out;
    }
    const out = await tool.run((args ?? {}) as Record<string, unknown>, this.d.backend);
    const result = { ok: out.ok, summary: out.summary, verified: out.findings.length === 0 || out.ok };
    if (out.ok) this.sink.emit('chat:tool', { tool: name, callId, args, phase: 'result', result });
    else this.sink.emit('chat:tool', { tool: name, callId, args, phase: 'result', error: out.summary });
    this.records.push({ label, tool: name, ok: out.ok, summary: out.summary, mutated: out.mutated });
    return out;
  }
}

// ---------------------------------------------------------------- routing

const routeSchema = z.object({ intents: z.array(z.enum(INTENTS)).min(1).max(3) });

async function route(run: Run, message: string): Promise<Route> {
  const quick = routeNoModel(message, run.history.slice(-4).map((t) => t.content).join(' '));
  if (quick) return quick;
  const { doc } = await run.state(null, false);
  let intents: Intent[] = ['clips'];
  try {
    const r = await run.d.planner.json({
      system: ROUTER_PROMPT,
      prompt: `${doc}

${recent(run.history)}REQUEST: ${message}`,
      schema: jsonSchemaOf(routeSchema),
      name: 'route',
      maxTokens: 100,
      signal: run.d.signal,
    });
    run.step(r.usage, '', []);
    const parsed = routeSchema.safeParse(r.value);
    if (parsed.success) intents = [...new Set(parsed.data.intents)];
  } catch (err) {
    if (run.d.signal?.aborted) throw err;
    // the classifier failing is not a reason to fail the request: the general clip executor can still try
  }
  // an instruction the router cannot map goes to the general executor (clip tools), not to a chat answer
  if (intents.every((i) => i === 'question') && !/\?/.test(message)) intents = ['clips'];
  const only = intents.length === 1 && intents[0] === 'question';
  return { intents, needsPlan: !only && needsPlan(intents), via: 'model' };
}

// ---------------------------------------------------------------- executor loop

interface LoopResult {
  ok: boolean;
  asked?: string;
}

async function execLoop(run: Run, goal: string, toolNames: string[], label: string, plan: EditPlan | null, hint = ''): Promise<LoopResult> {
  const tools = toolNames.map((n) => FACADE[n]).filter((t): t is FacadeTool => Boolean(t));
  const notes: string[] = [];
  let nudged = false;
  let rounds = 0;
  let followUps = 0;
  const compound = looksCompound(goal);
  while (rounds < MAX_ROUNDS) {
    if (run.d.signal?.aborted) return { ok: false };
    const { doc } = await run.state(plan);
    const prompt = [
      doc,
      '',
      recent(run.history).trimEnd(),
      `REQUEST: ${goal}`,
      hint,
      notes.length ? `SO FAR IN THIS STEP:\n${notes.join('\n')}` : '',
      `TOOLS: ${tools.map((t) => t.name).join(', ')}`,
    ]
      .filter((l) => l !== '')
      .join('\n');
    const r = await run.d.executor.tools({ system: EXECUTOR_SYSTEM, prompt, tools, signal: run.d.signal });
    run.step(r.usage, r.text, r.calls);
    if (r.calls.length === 0) {
      if (!nudged && !notes.some((n) => n.startsWith('OK'))) {
        nudged = true;
        notes.push('You answered without calling a tool. Call the tool now with sensible defaults; do not ask.');
        continue;
      }
      if (notes.some((n) => n.startsWith('OK'))) return { ok: true };
      return { ok: false, asked: r.text.trim() };
    }
    rounds++;
    let failed = false;
    for (const call of r.calls.slice(0, 4)) {
      if (!tools.some((t) => t.name === call.name)) {
        notes.push(`FAILED ${call.name}: not available here; use one of: ${tools.map((t) => t.name).join(', ')}.`);
        failed = true;
        continue;
      }
      if (call.invalid) {
        notes.push(`FAILED ${call.name}: ${call.invalid}`);
        failed = true;
        continue;
      }
      const out = await run.runTool(call.name, call.args, label);
      notes.push(`${out.ok ? 'OK' : 'FAILED'} ${call.name}: ${out.summary}`);
      if (!out.ok) failed = true;
    }
    // an action that found nothing to change is a fact, not an error; readers only: look again with their output
    if (!failed && r.calls.some((c) => FACADE[c.name]?.mutates)) {
      // one action was enough unless the request asks for several: then look again (state refreshed, what is done is listed)
      if (!compound || followUps >= 2) return { ok: true };
      followUps++;
      notes.push('If every part of the REQUEST is now done, answer DONE without calling a tool. Otherwise call a tool ONLY for the part not done yet; never repeat what is already done.');
      rounds--;
    }
  }
  return { ok: false };
}

// ---------------------------------------------------------------- plan steps

/** Mechanical steps map straight onto a facade tool (no LLM). */
export function mechanicalCall(step: PlanStep): { tool: string; args: Record<string, unknown> } | null {
  const p = step.params;
  switch (step.kind) {
    case 'remove_pauses':
      return { tool: 'removePauses', args: { minPauseSec: p.thresholdSec ?? 0.5 } };
    case 'remove_fillers':
      return { tool: 'removeFillers', args: p.words?.length ? { words: p.words } : {} };
    case 'remove_retakes':
      return { tool: 'removeRetakes', args: {} };
    case 'beat_cut':
      return { tool: 'cutToBeat', args: {} };
    case 'captions':
      return { tool: 'addCaptions', args: p.preset ? { preset: p.preset } : {} };
    case 'music':
      return { tool: 'addMusic', args: { asset: p.asset ?? '', volume: p.volume ?? 0.25, duck: p.duck ?? true } };
    case 'title':
      return { tool: 'addTitle', args: { text: p.text ?? '', atSec: p.startSec ?? 0, durationSec: p.durationSec ?? 3 } };
    case 'canvas':
      return { tool: 'setCanvas', args: { aspect: p.aspect } };
    case 'export':
      return { tool: 'exportVideo', args: { preset: p.exportPreset ?? '1080p' } };
    default:
      return null;
  }
}

const picksSchema = z.object({
  picks: z
    .array(z.object({ first_sentence: z.number().int().min(1), last_sentence: z.number().int().min(1) }))
    .min(1)
    .max(6),
});

/** Judgment: which part of the transcript to keep. The model picks sentence numbers; code makes them fit and builds the cut. */
async function assembleStep(run: Run, step: PlanStep | { goal: string; params: { topic?: string; targetSec?: number } }, plan: EditPlan | null): Promise<{ ok: boolean; summary: string }> {
  const label = `assemble: ${step.goal}`;
  const { snap } = await run.state(plan, false);
  const ids = speechAssetIds(snap);
  if (ids.length === 0) return { ok: false, summary: 'No footage with a transcript to build the cut from.' };
  // the speech asset with the most footage on the timeline (else the longest)
  const onTimeline = (id: string) => snap.doc.items.filter((i) => i.assetId === id).reduce((a, i) => a + i.durationFrames, 0);
  const assetId = [...ids].sort((a, b) => onTimeline(b) - onTimeline(a) || (snap.assets.find((x) => x.id === b)?.durationMs ?? 0) - (snap.assets.find((x) => x.id === a)?.durationMs ?? 0))[0]!;
  const asset = snap.assets.find((a) => a.id === assetId)!;
  const sentences = await snappedSentences(run.d.backend, snap, assetId);
  let listing = sentences.map((s) => `[${s.index}] ${fmtTime(s.startSec)}–${fmtTime(s.endSec)} ${s.text}`).join('\n');
  if (listing.length > 24000) listing = `${listing.slice(0, 24000)}\n... (transcript continues)`;
  const target = step.params.targetSec;
  let problem = '';
  for (let attempt = 0; attempt < 2; attempt++) {
    const { doc } = await run.state(plan, false);
    const callId = nextId();
    const args = { asset: asset.originalName, topic: step.params.topic, targetSec: target };
    run.sink.emit('chat:tool', { tool: 'pickSegments', callId, args, phase: 'call' });
    const r = await run.d.executor.json({
      system: 'You choose which parts of a transcript to keep for a video cut. Answer with sentence numbers only.',
      prompt: [
        doc,
        '',
        `TRANSCRIPT of "${asset.originalName}" (sentence number, seconds, text):`,
        listing,
        '',
        `REQUEST: ${step.goal}${step.params.topic ? ` Topic: ${step.params.topic}.` : ''}${target ? ` Target length: about ${target} seconds.` : ''}`,
        'Pick the sentence ranges to keep, in the order they should play. Only include sentences that are about the topic.',
        problem,
      ]
        .filter(Boolean)
        .join('\n'),
      schema: jsonSchemaOf(picksSchema),
      name: 'picks',
      maxTokens: 300,
      signal: run.d.signal,
    });
    run.step(r.usage, '', []);
    const parsed = picksSchema.safeParse(r.value);
    if (!parsed.success) {
      problem = 'PROBLEM: the picks were not valid; give 1-6 ranges of sentence numbers.';
      run.sink.emit('chat:tool', { tool: 'pickSegments', callId, args, phase: 'result', error: problem });
      continue;
    }
    const picks: SentencePick[] = parsed.data.picks.map((p) => ({ first: p.first_sentence, last: p.last_sentence }));
    const fit = fitPicks(sentences, picks, target);
    run.sink.emit('chat:tool', { tool: 'pickSegments', callId, args, phase: 'result', result: { picks, keptSeconds: Number(fit.totalSec.toFixed(1)) } });
    if (target && fit.totalSec < target * 0.7 && attempt === 0) {
      problem = `PROBLEM: those sentences only add up to ${fmtDur(fit.totalSec)}; pick more of the topic to reach about ${fmtDur(target)}.`;
      continue;
    }
    const out = await run.runTool(
      'assembleSequence',
      { ranges: fit.ranges.map((x) => ({ asset: asset.originalName, fromSec: Number(x.startSec.toFixed(2)), toSec: Number(x.endSec.toFixed(2)) })) },
      label,
    );
    if (out.ok && target && fit.totalSec < target * 0.6) return { ok: true, summary: `${out.summary} (shorter than the ${fmtDur(target)} asked for: that is all the footage on the topic)` };
    return { ok: out.ok, summary: out.summary };
  }
  return { ok: false, summary: 'Could not decide which parts to keep.' };
}

async function planSteps(run: Run, message: string, route: Route): Promise<EditPlan | null> {
  const { snap, doc } = await run.state(null, false);
  const stored = parseStoredPlan(run.d.backend.loadPlan());
  let errors: string[] = [];
  for (let attempt = 0; attempt < 2; attempt++) {
    const callId = nextId();
    run.sink.emit('chat:tool', { tool: 'plan', callId, args: { request: message }, phase: 'call' });
    try {
      const r = await run.d.planner.json({
        system: PLANNER_SYSTEM,
        prompt: [
          doc,
          '',
          recent(run.history).trimEnd(),
          stored ? `CURRENT PLAN (revise it if the request changes it, otherwise write a new plan):\n${renderPlan(stored)}\n` : '',
          `REQUEST: ${message}`,
          errors.length ? `PROBLEMS WITH YOUR LAST PLAN: ${errors.join('; ')}` : '',
        ]
          .filter((l) => l !== '')
          .join('\n'),
        schema: planJsonSchema(),
        name: 'edit_plan',
        maxTokens: 1200,
        signal: run.d.signal,
      });
      run.step(r.usage, '', []);
      const v = validatePlan(r.value, message, snap);
      if (v.plan) {
        run.sink.emit('chat:tool', { tool: 'plan', callId, args: { request: message }, phase: 'result', result: { summary: v.plan.summary, steps: v.plan.steps.map((s) => `${s.kind}: ${s.goal}`) } });
        void route;
        return v.plan;
      }
      errors = v.errors;
      run.sink.emit('chat:tool', { tool: 'plan', callId, args: { request: message }, phase: 'result', error: errors.join('; ') });
    } catch (err) {
      errors = [err instanceof Error ? err.message : String(err)];
      run.sink.emit('chat:tool', { tool: 'plan', callId, args: { request: message }, phase: 'result', error: errors[0] });
    }
  }
  return null;
}

async function runPlan(run: Run, plan: EditPlan): Promise<void> {
  const save = () => run.d.backend.savePlan(plan);
  save();
  for (const step of plan.steps) {
    if (run.d.signal?.aborted) return;
    step.status = 'running';
    save();
    const label = `${step.kind}: ${step.goal}`;
    let ok = false;
    let note = '';
    if (step.kind === 'assemble') {
      const r = await assembleStep(run, step, plan);
      ok = r.ok;
      note = r.summary;
    } else {
      const call = mechanicalCall(step);
      if (call) {
        const out = await run.runTool(call.tool, call.args, label);
        ok = out.ok;
        note = out.summary;
        if (!ok) {
          // retry the step with the specific error, through the executor, bounded
          const r = await execLoop(run, step.goal, [call.tool], label, plan, `A deterministic attempt with ${JSON.stringify(call.args)} failed: ${out.summary}. Fix the arguments.`);
          ok = r.ok;
          if (r.ok) note = run.records[run.records.length - 1]?.summary ?? note;
        }
      } else {
        const r = await execLoop(run, step.goal, toolsetFor(['clips']), label, plan);
        ok = r.ok;
        note = run.records[run.records.length - 1]?.summary ?? '';
        if (r.asked) note = r.asked;
      }
    }
    step.status = ok ? 'done' : 'failed';
    step.note = clip(note, 120);
    save();
  }
}

// ---------------------------------------------------------------- reply from verified facts

const cap = (s: string): string => (s ? s[0]!.toUpperCase() + s.slice(1) : s);

export function composeReply(records: StepRecord[], asked: string, plan: EditPlan | null): string {
  if (asked && records.every((r) => !r.ok)) return asked;
  if (records.length === 0) return asked || 'I could not work out what to change. Tell me which clip or part of the video you mean.';
  // the last record per (label, tool) wins; a fixed-up retry replaces its failed attempt
  const lines: string[] = [];
  const failed: StepRecord[] = [];
  for (const r of records) {
    if (r.ok) lines.push(`- ${cap(r.summary)}`);
    else failed.push(r);
  }
  const stillFailing = failed.filter((f) => !records.some((r) => r.ok && r.tool === f.tool && r.label === f.label));
  const out: string[] = [];
  if (lines.length > 0) out.push(plan ? `Done (${plan.steps.filter((s) => s.status === 'done').length}/${plan.steps.length} steps):\n${lines.join('\n')}` : lines.length === 1 ? lines[0]!.slice(2) + '.' : lines.join('\n'));
  for (const f of stillFailing) out.push(`I could not finish "${f.label}": ${f.summary}`);
  return out.join('\n');
}

// ---------------------------------------------------------------- entry

export async function runHarness(message: string, history: HistoryTurn[], deps: HarnessDeps): Promise<HarnessResult> {
  const run = new Run(deps, history);
  const b = deps.backend;
  let plan: EditPlan | null = null;
  let groupOpen = false;
  try {
    const r = await route(run, message);
    const pure = r.intents.every((i) => i === 'question');
    if (pure) {
      const { doc } = await run.state(null, true);
      const t = await deps.planner.text({ system: QUESTION_SYSTEM, prompt: `${doc}\n\n${recent(history)}${message}`, maxTokens: 300, signal: deps.signal });
      run.step(t.usage, t.text, []);
      const text = t.text.trim() || 'Which clip or part of the video do you mean?';
      deps.sink.emit('chat:delta', { text });
      return { reply: text, steps: [], asked: true };
    }
    const history0 = r.intents.every((i) => i === 'undo' || i === 'redo' || i === 'export');
    if (!history0) {
      b.beginGroup(clip(message, 60));
      groupOpen = true;
    }
    let looped: LoopResult | null = null;
    if (r.needsPlan) plan = await planSteps(run, message, r);
    if (plan) await runPlan(run, plan);
    else if (r.intents.includes('assemble') && !r.intents.includes('clips')) {
      const a = await assembleStep(run, { goal: message, params: {} }, null);
      if (!a.ok && run.records.length === 0) run.records.push({ label: message, tool: 'assembleSequence', ok: false, summary: a.summary, mutated: false });
    } else {
      looped = await execLoop(run, message, toolsetFor(r.intents), message, null);
    }
    const reply = composeReply(run.records, looped?.asked ?? '', plan);
    deps.sink.emit('chat:delta', { text: reply });
    return { reply, steps: run.records, asked: Boolean(looped?.asked) && run.records.every((x) => !x.ok) };
  } finally {
    if (groupOpen) b.endGroup();
  }
}
