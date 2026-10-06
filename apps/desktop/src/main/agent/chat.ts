import { z } from 'zod';
import { streamText, tool as aiTool, type LanguageModel } from 'ai';
import { createAnthropic } from '@ai-sdk/anthropic';
import { createOpenAI } from '@ai-sdk/openai';
import { createGoogleGenerativeAI } from '@ai-sdk/google';
import { AGENT_SYSTEM_PROMPT } from '@cutboard/agent';
import { getSettings, getAgentKey, type AiSettings } from '../settings.ts';
import { callTool, registry } from '../tools-bridge.ts';
import { broadcast } from '../events.ts';

/**
 * Built-in agent (addendum §2, main prompt §6): Vercel AI SDK streaming with the shared
 * tool registry as its hands. Provider adapters: Anthropic / OpenAI / Google / Ollama
 * (OpenAI-compatible). Keys live in the OS-encrypted secret store. Streams narration,
 * tool calls, and tool results to the chat UI over the event bus.
 */

export interface ChatMessage {
  role: 'user' | 'assistant';
  content: string;
}

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
    case 'ollama': {
      // OpenAI-compatible endpoint served by Ollama (local, keyless). Local servers speak
      // Chat Completions, not the Responses API createOpenAI defaults to, hence .chat().
      const ollama = createOpenAI({ baseURL: `${trimSlash(ai.ollamaUrl ?? 'http://127.0.0.1:11434')}/v1`, apiKey: 'ollama' });
      return ollama.chat(model || 'qwen2.5:7b');
    }
    case 'anthropic':
    default: {
      const anthropic = createAnthropic({ apiKey: key });
      return anthropic(model || 'claude-3-5-haiku-latest');
    }
  }
}

const aborts = new Map<string, AbortController>();

export async function sendChatMessage(chatId: string, userMessage: string): Promise<void> {
  const settings = await getSettings();
  const provider = settings.ai?.agentProvider ?? 'anthropic';
  const model = settings.ai?.agentModel ?? '';
  const key = (await getAgentKey(provider)) ?? '';
  if (provider !== 'ollama' && !key) {
    broadcastChat(chatId, 'chat:done', {
      error: `No API key configured for ${provider}. Open Settings → AI and add one (or switch to Ollama for a local model).`,
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
      execute: async (args: unknown) => {
        broadcastChat(chatId, 'chat:tool', { tool: tool.name, args, phase: 'call' });
        try {
          const result = await callTool(tool.name, args, 'builtin-agent');
          broadcastChat(chatId, 'chat:tool', { tool: tool.name, args, phase: 'result', result });
          return result;
        } catch (err) {
          const message = err instanceof Error ? err.message : String(err);
          const hint = (err as { hint?: string }).hint;
          broadcastChat(chatId, 'chat:tool', { tool: tool.name, args, phase: 'result', error: message });
          return { error: hint ? `${message} (hint: ${hint})` : message };
        }
      },
    });
  }

  const controller = new AbortController();
  aborts.set(chatId, controller);

  try {
    // streamText reports failures (e.g. a local server that isn't running) via onError
    // instead of throwing, which would otherwise end the reply silently
    let streamError: unknown;
    const result = streamText({
      model: languageModel,
      system: AGENT_SYSTEM_PROMPT,
      messages: [{ role: 'user' as const, content: userMessage }],
      tools,
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
    broadcastChat(chatId, 'chat:done', { finishReason: finish });
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    broadcastChat(chatId, 'chat:done', { error: message });
  } finally {
    aborts.delete(chatId);
  }
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
