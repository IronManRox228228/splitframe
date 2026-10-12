import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { z } from 'zod';
import { agentKeySlot } from './settings.ts';
import { harnessSupported } from './agent/harness/index.ts';
import { createLlm } from './agent/harness/llm.ts';
import { friendlyError } from '../renderer/editor/ChatTab.tsx';
import { PROVIDER_LABELS } from '../renderer/editor/AiSettings.tsx';

describe('Meterless inference routing (Milestone H1)', () => {
  describe('Provider labels and key slots', () => {
    it('registers Meterless in PROVIDER_LABELS', () => {
      expect(PROVIDER_LABELS['meterless']).toBe('Meterless');
    });

    it('isolates Meterless API keys to agentKey:meterless slot', () => {
      expect(agentKeySlot('meterless')).toBe('agentKey:meterless');
      expect(agentKeySlot('openai')).toBe('agentKey:openai');
      expect(agentKeySlot('anthropic')).toBe('agentKey:anthropic');
      expect(agentKeySlot('meterless')).not.toBe(agentKeySlot('openai'));
    });
  });

  describe('IPC Zod Schemas for Meterless', () => {
    const aiSetConfigSchema = z.object({
      agentProvider: z.enum(['anthropic', 'openai', 'google', 'openrouter', 'ollama', 'llamacpp', 'meterless']).optional(),
      agentModel: z.string().max(120).optional(),
      harness: z.enum(['v1', 'classic']).optional(),
      plannerModel: z.string().max(120).optional(),
      executorModel: z.string().max(120).optional(),
      vlmProvider: z.enum(['none', 'ollama', 'anthropic', 'openai']).optional(),
      vlmModel: z.string().max(120).optional(),
      ollamaUrl: z.string().url().optional(),
      llamacppUrl: z.string().url().optional(),
      meterlessUrl: z.string().url().optional(),
      agentKey: z.string().max(400).optional(),
      llamacppKey: z.string().max(400).optional(),
      anthropicKey: z.string().max(400).optional(),
      openaiKey: z.string().max(400).optional(),
    });

    it('validates configuration with meterless provider and standard URL', () => {
      const valid = aiSetConfigSchema.parse({
        agentProvider: 'meterless',
        meterlessUrl: 'https://api.meterless.ai/v1',
        agentModel: 'qwen-2.5-coder-32b',
        agentKey: 'mtl_live_secretkey12345',
      });
      expect(valid.agentProvider).toBe('meterless');
      expect(valid.meterlessUrl).toBe('https://api.meterless.ai/v1');
      expect(valid.agentModel).toBe('qwen-2.5-coder-32b');
    });

    it('validates custom self-hosted or proxy meterless endpoints', () => {
      const valid = aiSetConfigSchema.parse({
        agentProvider: 'meterless',
        meterlessUrl: 'http://127.0.0.1:8000/v1',
      });
      expect(valid.meterlessUrl).toBe('http://127.0.0.1:8000/v1');
    });

    it('rejects invalid or non-URL meterlessUrl strings', () => {
      expect(() =>
        aiSetConfigSchema.parse({
          agentProvider: 'meterless',
          meterlessUrl: 'not-a-valid-url',
        }),
      ).toThrow();
    });

    it('rejects invalid or unsupported agent providers', () => {
      expect(() =>
        aiSetConfigSchema.parse({
          agentProvider: 'unsupported-provider' as never,
        }),
      ).toThrow();
    });
  });

  describe('createLlm with Meterless OpenAI-compatible endpoint', () => {
    const originalFetch = globalThis.fetch;

    afterEach(() => {
      globalThis.fetch = originalFetch;
      vi.restoreAllMocks();
    });

    it('normalizes base URLs without double /v1/v1', async () => {
      let calledUrl = '';
      let requestHeaders: Record<string, string> = {};
      let requestBody: Record<string, unknown> = {};

      globalThis.fetch = vi.fn().mockImplementation(async (url: string, init?: RequestInit) => {
        calledUrl = url;
        requestHeaders = init?.headers as Record<string, string>;
        requestBody = JSON.parse(init?.body as string);
        return {
          ok: true,
          status: 200,
          json: async () => ({
            choices: [{ message: { content: 'test response' } }],
            usage: { prompt_tokens: 15, completion_tokens: 5 },
          }),
        } as unknown as Response;
      });

      // Pass URL with /v1
      const llm = createLlm({
        baseUrl: 'https://api.meterless.ai/v1',
        apiKey: 'mtl-secret-key',
        model: 'qwen-2.5-coder-32b',
        noThinking: false,
      });

      const res = await llm.text({ system: 'sys', prompt: 'hello' });
      expect(res.text).toBe('test response');
      expect(calledUrl).toBe('https://api.meterless.ai/v1/chat/completions');
      expect(requestHeaders['Authorization']).toBe('Bearer mtl-secret-key');
      expect(requestBody.model).toBe('qwen-2.5-coder-32b');
      expect(res.usage).toEqual({ inputTokens: 15, outputTokens: 5 });
    });

    it('normalizes base URLs without trailing /v1', async () => {
      let calledUrl = '';
      globalThis.fetch = vi.fn().mockImplementation(async (url: string) => {
        calledUrl = url;
        return {
          ok: true,
          status: 200,
          json: async () => ({
            choices: [{ message: { content: 'ok' } }],
          }),
        } as unknown as Response;
      });

      const llm = createLlm({
        baseUrl: 'https://api.meterless.ai',
        apiKey: 'key',
        model: 'llama-3.3-70b',
        noThinking: false,
      });

      await llm.text({ system: '', prompt: 'ping' });
      expect(calledUrl).toBe('https://api.meterless.ai/v1/chat/completions');
    });

    it('handles JSON schema structured mode against Meterless', async () => {
      globalThis.fetch = vi.fn().mockImplementation(async () => {
        return {
          ok: true,
          status: 200,
          json: async () => ({
            choices: [{ message: { content: JSON.stringify({ decision: 'cut', atMs: 1200 }) } }],
            usage: { prompt_tokens: 40, completion_tokens: 12 },
          }),
        } as unknown as Response;
      });

      const llm = createLlm({
        baseUrl: 'https://api.meterless.ai/v1',
        apiKey: 'key',
        model: 'deepseek-r1',
        noThinking: false,
      });

      const result = await llm.json({
        system: 'You are a video editor.',
        prompt: 'Should we cut?',
        name: 'cut_decision',
        schema: { type: 'object', properties: { decision: { type: 'string' }, atMs: { type: 'number' } } },
      });

      expect(result.value).toEqual({ decision: 'cut', atMs: 1200 });
      expect(result.usage.inputTokens).toBe(40);
    });

    it('throws error when Meterless returns 401 unauthorized', async () => {
      globalThis.fetch = vi.fn().mockImplementation(async () => {
        return {
          ok: false,
          status: 401,
          json: async () => ({
            error: { message: 'Incorrect or expired Meterless API key.' },
          }),
        } as unknown as Response;
      });

      const llm = createLlm({
        baseUrl: 'https://api.meterless.ai/v1',
        apiKey: 'invalid-key',
        model: 'qwen-2.5-coder-32b',
        noThinking: false,
      });

      await expect(llm.text({ system: '', prompt: 'hi' })).rejects.toThrow('Incorrect or expired Meterless API key.');
    });

    it('throws error when Meterless returns 429 rate limit', async () => {
      globalThis.fetch = vi.fn().mockImplementation(async () => {
        return {
          ok: false,
          status: 429,
          json: async () => ({
            error: { message: 'Rate limit exceeded: concurrent requests cap reached.' },
          }),
        } as unknown as Response;
      });

      const llm = createLlm({
        baseUrl: 'https://api.meterless.ai/v1',
        apiKey: 'key',
        model: 'qwen-2.5-coder-32b',
        noThinking: false,
      });

      await expect(llm.text({ system: '', prompt: 'hi' })).rejects.toThrow('Rate limit exceeded: concurrent requests cap reached.');
    });
  });

  describe('friendlyError for Meterless connection issues', () => {
    it('formats network failure for Meterless with endpoint URL', () => {
      const err = friendlyError('TypeError: fetch failed (ECONNREFUSED)', {
        agentProvider: 'meterless',
        meterlessUrl: 'https://api.meterless.ai/v1',
        agentModel: '',
        asrModel: 'base.en',
        vlmProvider: 'none',
        llamacppUrl: '',
        ollamaUrl: '',
      });
      expect(err.connection).toBe(true);
      expect(err.message).toBe("Can't reach Meterless at https://api.meterless.ai/v1. Check your internet connection and try again.");
    });

    it('formats 401 rejected API key error for Meterless', () => {
      const err = friendlyError('401 Unauthorized: Invalid API key', {
        agentProvider: 'meterless',
        meterlessUrl: 'https://api.meterless.ai/v1',
        agentModel: '',
        asrModel: 'base.en',
        vlmProvider: 'none',
        llamacppUrl: '',
        ollamaUrl: '',
      });
      expect(err.connection).toBe(false);
      expect(err.message).toBe('The provider rejected the API key. Check it in AI settings.');
    });

    it('formats 429 quota/rate limit error for Meterless', () => {
      const err = friendlyError('429 Too Many Requests: Rate limit reached', {
        agentProvider: 'meterless',
        meterlessUrl: 'https://api.meterless.ai/v1',
        agentModel: '',
        asrModel: 'base.en',
        vlmProvider: 'none',
        llamacppUrl: '',
        ollamaUrl: '',
      });
      expect(err.connection).toBe(false);
      expect(err.message).toBe('The provider is rate-limiting requests or the quota is used up. Wait a moment and retry.');
    });
  });
});
