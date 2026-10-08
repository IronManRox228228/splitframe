import { getSettings, getSecret, getAgentKey } from '../../settings.ts';
import type { ChatSink } from '../chat.ts';
import { appBackend } from './backend.ts';
import { createLlm, type Llm } from './llm.ts';
import { runHarness, type HistoryTurn } from './run.ts';

export { runHarness } from './run.ts';

/** Providers the harness can drive: OpenAI-compatible chat completions with JSON-schema output and tool calls. */
const SUPPORTED = new Set(['llamacpp', 'ollama', 'openai']);

export async function harnessSupported(): Promise<boolean> {
  const s = await getSettings();
  return SUPPORTED.has(s.ai?.agentProvider ?? 'anthropic');
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
  throw new Error(`The v1 harness does not support the ${provider} provider; switch to the classic harness in settings.`);
}

/** One chat turn through harness v1; same contract as the classic runChatTurn (throws on failure). */
export async function runHarnessTurn(
  _chatId: string,
  userMessage: string,
  history: HistoryTurn[],
  controller: AbortController,
  sink: ChatSink,
): Promise<void> {
  const planner = await llmFor('planner');
  const executor = await llmFor('executor');
  try {
    await runHarness(userMessage, history, { backend: appBackend(), planner, executor, sink, signal: controller.signal });
    sink.emit('chat:done', { finishReason: 'stop' });
  } catch (err) {
    if (controller.signal.aborted) {
      sink.emit('chat:done', { finishReason: 'abort' });
      return;
    }
    throw err;
  }
}
