import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { getSettings, getSecret, getAgentKey } from '../../settings.ts';
import type { ChatSink } from '../chat.ts';
import { appBackend } from './backend.ts';
import { createLlm, type Llm } from './llm.ts';
import { runHarness, runStoredPlan, type HarnessDeps, type HistoryTurn } from './run.ts';
import { getProjectMode } from '../mode-store.ts';
import { waitForDecision } from '../pending.ts';

export { runHarness } from './run.ts';

/** Providers the harness can drive: OpenAI-compatible chat completions with JSON-schema output and tool calls. */
const SUPPORTED = new Set(['llamacpp', 'ollama', 'openai', 'meterless']);

export async function harnessSupported(): Promise<boolean> {
  const s = await getSettings();
  return SUPPORTED.has(s.ai?.agentProvider ?? 'anthropic');
}

/**
 * Meterless runtime token (Part A item 2): METERLESS_RUNTIME_TOKEN env wins, then the key
 * saved encrypted from AI settings, then the runtime's own proxy-token file. The token is
 * only ever sent as the Bearer value — never displayed, logged or persisted elsewhere.
 */
async function meterlessRuntimeToken(): Promise<string> {
  const fromEnv = process.env.METERLESS_RUNTIME_TOKEN?.trim();
  if (fromEnv) return fromEnv;
  const fromStore = (await getAgentKey('meterless')) ?? '';
  if (fromStore.trim()) return fromStore.trim();
  try {
    const localAppData = process.env.LOCALAPPDATA;
    if (localAppData) {
      const fromRuntime = (await readFile(join(localAppData, 'ai.meterless.runtime', 'proxy-token'), 'utf8')).trim();
      if (fromRuntime) return fromRuntime;
    }
  } catch {
    // no runtime token on disk: only a failure once the caller needs one
  }
  return '';
}

async function llmFor(role: 'planner' | 'executor'): Promise<Llm> {
  const s = await getSettings();
  const ai = s.ai ?? {};
  const provider = ai.agentProvider ?? 'anthropic';
  // planner and executor models are separate settings; both default to the agent model
  const model = (role === 'planner' ? ai.plannerModel : ai.executorModel) || ai.agentModel || '';
  const trim = (u: string) => u.replace(/\/+$/, '');
  if (provider === 'llamacpp') {
    return createLlm({ baseUrl: trim(ai.llamacppUrl ?? 'http://127.0.0.1:8080'), apiKey: (await getSecret('llamacppKey')) ?? '', model: model || 'default', noThinking: true });
  }
  if (provider === 'ollama') return createLlm({ baseUrl: trim(ai.ollamaUrl ?? 'http://127.0.0.1:11434'), apiKey: 'ollama', model: model || 'qwen2.5:7b', noThinking: false });
  if (provider === 'openai') return createLlm({ baseUrl: 'https://api.openai.com', apiKey: (await getAgentKey('openai')) ?? '', model: model || 'gpt-4o-mini', noThinking: false });
  if (provider === 'meterless') {
    // No invented endpoints (Part A item 2): an unset endpoint means unconfigured, and it
    // fails loudly here instead of silently talking to api.meterless.ai.
    const raw = ai.meterlessUrl?.trim() || process.env.METERLESS_BASE_URL?.trim() || '';
    if (!raw) throw new Error('Meterless is selected but no endpoint is configured; set it in AI settings or METERLESS_BASE_URL.');
    const baseUrl = trim(raw).replace(/\/v1$/, '');
    const token = await meterlessRuntimeToken();
    if (!token) throw new Error('Meterless needs the runtime token: set METERLESS_RUNTIME_TOKEN or save it in AI settings.');
    // Routing mode (Part A item 4): the saved routing mode, then METERLESS_MODEL (a single
    // routing mode covers all lanes here); 'default' is only the runtime's own alias.
    const routingMode = ai.meterlessModel?.trim() || process.env.METERLESS_MODEL?.trim() || model;
    return createLlm({ baseUrl, apiKey: token, model: routingMode || 'default', noThinking: false, meterless: true });
  }
  throw new Error(`The v1 harness does not support the ${provider} provider; switch to the classic harness in settings.`);
}

/** One chat turn through harness v1; same contract as the classic runChatTurn (throws on failure). */
export async function runHarnessTurn(
  _chatId: string,
  userMessage: string,
  history: HistoryTurn[],
  controller: AbortController,
  sink: ChatSink,
  opts: { revise?: boolean; mode?: HarnessDeps['mode']; confirm?: HarnessDeps['confirm'] } = {},
): Promise<void> {
  const planner = await llmFor('planner');
  const executor = await llmFor('executor');
  try {
    await runHarness(userMessage, history, {
      backend: appBackend(),
      planner,
      executor,
      sink,
      signal: controller.signal,
      mode: opts.mode ?? getProjectMode(),
      confirm: opts.confirm ?? ((req) => waitForDecision(req.id, controller.signal)),
      revise: opts.revise,
    });
    sink.emit('chat:done', { finishReason: 'stop' });
  } catch (err) {
    if (controller.signal.aborted) {
      sink.emit('chat:done', { finishReason: 'abort' });
      return;
    }
    throw err;
  }
}

/** The user pressed Run on the plan card. */
export async function runHarnessPlanTurn(controller: AbortController, sink: ChatSink): Promise<void> {
  const planner = await llmFor('planner');
  const executor = await llmFor('executor');
  try {
    await runStoredPlan({
      backend: appBackend(),
      planner,
      executor,
      sink,
      signal: controller.signal,
      mode: getProjectMode(),
      confirm: (req) => waitForDecision(req.id, controller.signal),
    });
    sink.emit('chat:done', { finishReason: 'stop' });
  } catch (err) {
    if (controller.signal.aborted) {
      sink.emit('chat:done', { finishReason: 'abort' });
      return;
    }
    throw err;
  }
}
