import { z } from 'zod';
import { streamText, stepCountIs, tool as aiTool, type LanguageModel } from 'ai';
import { createAnthropic } from '@ai-sdk/anthropic';
import { createOpenAI } from '@ai-sdk/openai';
import { createGoogleGenerativeAI } from '@ai-sdk/google';
import { AGENT_SYSTEM_PROMPT } from '@cutboard/agent';
import { getSettings, getSecret, getAgentKey, type AiSettings } from '../settings.ts';
import { callTool, registry } from '../tools-bridge.ts';
import { broadcast } from '../events.ts';
import { editorContextCache } from '../editor-context.ts';
import { buildMessages, formatEditorContext, type ChatTurn } from './messages.ts';
import { splitImageResult, stripImageData } from './tool-output.ts';

/**
 * Built-in agent (addendum §2, main prompt §6): Vercel AI SDK streaming with the shared
 * tool registry as its hands. Provider adapters: Anthropic / OpenAI / Google, plus local
 * Ollama and llama.cpp `llama-server` (both via their OpenAI-compatible /v1 endpoints). Keys live in the OS-encrypted secret store. Streams narration,
 * tool calls, and tool results to the chat UI over the event bus.
 */

export interface ChatMessage {
  role: 'user' | 'assistant';
  content: string;
}

const LOCAL_PROVIDERS = new Set(['ollama', 'llamacpp']);

/** Tool-call rounds per user message (read, edit, verify, ...); one round was too few to finish an edit. */
const MAX_AGENT_STEPS = 12;

function providerModel(provider: string, model: string, key: string, ai: AiSettings): LanguageModel {
  switch (provider) {
    case 'openai': {
      const openai = createOpenAI({ apiKey: key });
      return openai(model || 'gpt-4o-mini');
    }
    case 'google': {
      const google = createGoogleGenerativeAI({ apiKey: key });
      return google(model || 'gemini-2.0-flash');
    }
    // Local servers speak Chat Completions, not the Responses API createOpenAI defaults to,
    // hence .chat().
    case 'ollama': {
      const ollama = createOpenAI({ baseURL: `${trimSlash(ai.ollamaUrl ?? 'http://127.0.0.1:11434')}/v1`, apiKey: 'ollama' });
      return ollama.chat(model || 'qwen2.5:7b');
    }
    case 'llamacpp': {
      // llama-server serves whatever model it was launched with and ignores the name
      // (router mode uses it to pick a model). Key only matters if started with --api-key.
      const llamacpp = createOpenAI({ baseURL: `${trimSlash(ai.llamacppUrl ?? 'http://127.0.0.1:8080')}/v1`, apiKey: key || 'llamacpp' });
      return llamacpp.chat(model || 'default');
    }
    case 'anthropic':
    default: {
      const anthropic = createAnthropic({ apiKey: key });
      return anthropic(model || 'claude-3-5-haiku-latest');
    }
  }
}

const aborts = new Map<string, AbortController>();

export async function sendChatMessage(chatId: string, userMessage: string, history: ChatTurn[] = []): Promise<void> {
  // everything below runs inside the try: a failure while setting up (bad settings, an
  // invalid server URL) must still end the turn in the UI instead of leaving it spinning
  aborts.get(chatId)?.abort();
  const controller = new AbortController();
  aborts.set(chatId, controller);
  try {
    await runChatTurn(chatId, userMessage, history, controller);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    broadcastChat(chatId, 'chat:done', { error: message });
  } finally {
    if (aborts.get(chatId) === controller) aborts.delete(chatId);
  }
}

async function runChatTurn(chatId: string, userMessage: string, history: ChatTurn[], controller: AbortController): Promise<void> {
  const settings = await getSettings();
  const provider = settings.ai?.agentProvider ?? 'anthropic';
  const model = settings.ai?.agentModel ?? '';
  // llama.cpp gets its own key slot so a cloud key is never sent to a self-hosted URL
  const key = (provider === 'llamacpp' ? await getSecret('llamacppKey') : await getAgentKey(provider)) ?? '';
  if (!LOCAL_PROVIDERS.has(provider) && !key) {
    broadcastChat(chatId, 'chat:done', {
      error: `No API key configured for ${provider}. Open Settings → AI and add one (or switch to Ollama / llama.cpp for a local model).`,
    });
    return;
  }

  const languageModel = providerModel(provider, model, key, settings.ai ?? {});

  // map the shared registry onto AI SDK tools
  // (typed loosely: the zod schema is the real contract and is validated by the registry)
  /* eslint-disable @typescript-eslint/no-explicit-any */
  const tools: Record<string, any> = {};
  for (const tool of registry.list()) {
    const shape = 'shape' in tool.input ? (tool.input as z.ZodObject).shape : {};
    tools[tool.name] = aiTool({
      description: tool.description,
      inputSchema: z.object(shape as never),
      execute: async (args: unknown, options?: { toolCallId?: string }) => {
        const callId = options?.toolCallId;
        broadcastChat(chatId, 'chat:tool', { tool: tool.name, callId, args, phase: 'call' });
        try {
          const result = await callTool(tool.name, args, 'builtin-agent');
          // the UI only needs to know an image was returned, not receive its bytes
          broadcastChat(chatId, 'chat:tool', { tool: tool.name, callId, args, phase: 'result', result: stripImageData(result) });
          return result;
        } catch (err) {
          const message = err instanceof Error ? err.message : String(err);
          const hint = (err as { hint?: string }).hint;
          broadcastChat(chatId, 'chat:tool', { tool: tool.name, callId, args, phase: 'result', error: message });
          return { error: hint ? `${message} (hint: ${hint})` : message };
        }
      },
      // images (captureFrame) go to the model as images, not as megabytes of base64 text
      toModelOutput: ({ output }: { output: unknown }) => {
        const { image, rest } = splitImageResult(output);
        if (!image) return { type: 'json', value: output } as never;
        const text = JSON.stringify(rest);
        if (LOCAL_PROVIDERS.has(provider)) return { type: 'text', value: `${text} (image not sent: local models are text-only here)` } as never;
        return {
          type: 'content',
          value: [
            { type: 'text', text },
            { type: 'file', data: { type: 'data', data: image.data }, mediaType: image.mimeType },
          ],
        } as never;
      },
    });
  }

  // streamText reports failures (e.g. a local server that isn't running) via onError
  // instead of throwing, which would otherwise end the reply silently
  let streamError: unknown;
  const result = streamText({
    model: languageModel,
    system: AGENT_SYSTEM_PROMPT,
    messages: buildMessages(history, userMessage, formatEditorContext(editorContextCache.get())),
    tools,
    // keep going through tool results until the model has finished (or the step budget runs out)
    stopWhen: stepCountIs(MAX_AGENT_STEPS),
    maxOutputTokens: 4000,
    abortSignal: controller.signal,
    onError: ({ error }) => {
      streamError = error;
    },
  });
  for await (const chunk of result.textStream) {
    broadcastChat(chatId, 'chat:delta', { text: chunk });
  }
  if (streamError) throw streamError;
  const finish = await result.finishReason;
  broadcastChat(chatId, 'chat:done', {
    finishReason: finish,
    ...(finish === 'tool-calls' ? { note: `Stopped after ${MAX_AGENT_STEPS} steps. Say "continue" to let the agent keep going.` } : {}),
  });
}

export function abortChat(chatId: string): void {
  aborts.get(chatId)?.abort();
}

function trimSlash(url: string): string {
  return url.replace(/\/+$/, '');
}

function broadcastChat(chatId: string, type: string, payload: unknown): void {
  broadcast('event', { type, payload: { chatId, ...(payload as Record<string, unknown>) } });
}
