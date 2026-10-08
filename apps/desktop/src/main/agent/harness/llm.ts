import { z } from 'zod';
import type { FacadeTool } from './facade.ts';

/**
 * Model access for the harness: OpenAI-compatible chat completions (llama.cpp, Ollama, OpenAI)
 * with grammar-constrained JSON (response_format json_schema) and tool calling. Calls are made
 * strictly one at a time by the callers. On llama.cpp the model's thinking mode is switched off:
 * every call here is a small, scoped decision and the thinking tokens cost far more than they help.
 */

export interface LlmConfig {
  baseUrl: string;
  apiKey: string;
  model: string;
  /** send chat_template_kwargs.enable_thinking=false (llama.cpp only) */
  noThinking: boolean;
}

export interface ToolCallOut {
  name: string;
  args: unknown;
  /** set when the arguments were not valid JSON */
  invalid?: string;
}

export interface LlmUsage {
  inputTokens?: number;
  outputTokens?: number;
}

export interface Llm {
  /** schema-constrained JSON; throws when the reply is not valid JSON */
  json(opts: { system: string; prompt: string; schema: Record<string, unknown>; name: string; maxTokens?: number; signal?: AbortSignal }): Promise<{ value: unknown; usage: LlmUsage }>;
  /** one round of tool calling: the calls the model made (none = it answered in text) */
  tools(opts: { system: string; prompt: string; tools: FacadeTool[]; maxTokens?: number; signal?: AbortSignal }): Promise<{ text: string; calls: ToolCallOut[]; usage: LlmUsage }>;
  text(opts: { system: string; prompt: string; maxTokens?: number; signal?: AbortSignal }): Promise<{ text: string; usage: LlmUsage }>;
}

export function jsonSchemaOf(schema: z.ZodType): Record<string, unknown> {
  const out = z.toJSONSchema(schema, { target: 'draft-7' }) as Record<string, unknown>;
  delete out['$schema'];
  return out;
}

interface ChatResponse {
  choices?: { message?: { content?: string | null; tool_calls?: { function?: { name?: string; arguments?: string } }[] } }[];
  usage?: { prompt_tokens?: number; completion_tokens?: number };
  error?: { message?: string };
}

export function createLlm(cfg: LlmConfig): Llm {
  const base = cfg.baseUrl.replace(/\/+$/, '');

  async function chat(body: Record<string, unknown>, signal?: AbortSignal): Promise<ChatResponse> {
    const res = await fetch(`${base}/v1/chat/completions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${cfg.apiKey || 'none'}` },
      body: JSON.stringify({ model: cfg.model || 'default', temperature: 0.2, ...(cfg.noThinking ? { chat_template_kwargs: { enable_thinking: false } } : {}), ...body }),
      ...(signal ? { signal } : {}),
    });
    const json = (await res.json().catch(() => ({}))) as ChatResponse;
    if (!res.ok) throw new Error(json.error?.message ?? `model server answered ${res.status}`);
    return json;
  }
  const usageOf = (r: ChatResponse): LlmUsage => ({ inputTokens: r.usage?.prompt_tokens, outputTokens: r.usage?.completion_tokens });
  const messages = (system: string, prompt: string) => [
    { role: 'system', content: system },
    { role: 'user', content: prompt },
  ];

  return {
    async json({ system, prompt, schema, name, maxTokens, signal }) {
      const r = await chat(
        { messages: messages(system, prompt), max_tokens: maxTokens ?? 1500, response_format: { type: 'json_schema', json_schema: { name, strict: true, schema } } },
        signal,
      );
      const content = r.choices?.[0]?.message?.content ?? '';
      try {
        return { value: JSON.parse(content), usage: usageOf(r) };
      } catch {
        throw new Error(`the model did not return valid JSON for ${name}: ${content.slice(0, 120)}`);
      }
    },
    async tools({ system, prompt, tools, maxTokens, signal }) {
      const r = await chat(
        {
          messages: messages(system, prompt),
          max_tokens: maxTokens ?? 1200,
          tool_choice: 'auto',
          tools: tools.map((t) => ({ type: 'function', function: { name: t.name, description: t.description, parameters: jsonSchemaOf(t.input) } })),
        },
        signal,
      );
      const msg = r.choices?.[0]?.message;
      const calls: ToolCallOut[] = (msg?.tool_calls ?? []).map((c) => {
        const name = c.function?.name ?? '';
        try {
          return { name, args: c.function?.arguments ? JSON.parse(c.function.arguments) : {} };
        } catch {
          return { name, args: {}, invalid: `arguments were not valid JSON: ${(c.function?.arguments ?? '').slice(0, 100)}` };
        }
      });
      return { text: msg?.content ?? '', calls, usage: usageOf(r) };
    },
    async text({ system, prompt, maxTokens, signal }) {
      const r = await chat({ messages: messages(system, prompt), max_tokens: maxTokens ?? 600 }, signal);
      return { text: r.choices?.[0]?.message?.content ?? '', usage: usageOf(r) };
    },
  };
}
