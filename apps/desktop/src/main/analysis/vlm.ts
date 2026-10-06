import { readFile } from 'node:fs/promises';
import { getSettings, saveSettings, saveSecret, getSecret, type VlmProvider } from '../settings.ts';

/**
 * Pluggable VLM scene descriptions (main prompt §5, addendum §3): local Ollama
 * (llava / qwen2.5-vl) or cloud Anthropic/OpenAI with the user's key. Privacy rule
 * (addendum §5.5): keyframes for THIS capability leave the machine only when the user
 * configured a cloud provider; Ollama stays local.
 */

export async function getVlmConfig(): Promise<{ provider: VlmProvider; model: string; ollamaUrl: string }> {
  const settings = await getSettings();
  return {
    provider: settings.ai?.vlmProvider ?? 'none',
    model: settings.ai?.vlmModel ?? (settings.ai?.vlmProvider === 'anthropic' ? 'claude-3-5-haiku-latest' : settings.ai?.vlmProvider === 'openai' ? 'gpt-4o-mini' : 'llava'),
    ollamaUrl: settings.ai?.ollamaUrl ?? 'http://127.0.0.1:11434',
  };
}

export async function setVlmConfig(patch: { vlmProvider?: VlmProvider; vlmModel?: string; ollamaUrl?: string; agentKey?: string; anthropicKey?: string; openaiKey?: string }): Promise<void> {
  const settings = await getSettings();
  // keys never go into settings.json; also scrub any plaintext agentKey an older build wrote
  delete (settings.ai as Record<string, unknown> | undefined)?.agentKey;
  const ai = { ...settings.ai, ...patch };
  delete ai.agentKey;
  delete ai.anthropicKey;
  delete ai.openaiKey;
  if (patch.agentKey) await saveSecret('agentKey', patch.agentKey);
  if (patch.anthropicKey) await saveSecret('anthropicKey', patch.anthropicKey);
  if (patch.openaiKey) await saveSecret('openaiKey', patch.openaiKey);
  await saveSettings({ ai });
}

/** Fetch one keyframe, base64 it, ask the provider for a dense visual description. */
export async function describeKeyframe(
  imagePath: string,
  contextHint: string,
): Promise<string> {
  const { provider, model, ollamaUrl } = await getVlmConfig();
  if (provider === 'none') throw new Error('No VLM provider configured (Settings → AI).');
  const base64 = (await readFile(imagePath)).toString('base64');
  const prompt =
    `Describe this video frame for a searchable video-editing index in one dense sentence: who is in frame, what they are doing, the setting, camera framing, and any on-screen text. ${contextHint}`;

  if (provider === 'ollama') {
    const res = await fetch(`${ollamaUrl}/api/chat`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        model,
        stream: false,
        messages: [{ role: 'user', content: prompt, images: [base64] }],
      }),
    });
    if (!res.ok) throw new Error(`Ollama error ${res.status}: ${await res.text()}`);
    const data = (await res.json()) as { message?: { content?: string } };
    return data.message?.content?.trim() ?? '';
  }

  if (provider === 'anthropic') {
    const key = await getSecret('anthropicKey');
    if (!key) throw new Error('Anthropic API key not set (Settings → AI).');
    const res = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: { 'content-type': 'application/json', 'x-api-key': key, 'anthropic-version': '2023-06-01' },
      body: JSON.stringify({
        model,
        max_tokens: 200,
        messages: [
          {
            role: 'user',
            content: [
              { type: 'image', source: { type: 'base64', media_type: 'image/jpeg', data: base64 } },
              { type: 'text', text: prompt },
            ],
          },
        ],
      }),
    });
    if (!res.ok) throw new Error(`Anthropic error ${res.status}: ${await res.text()}`);
    const data = (await res.json()) as { content?: { text?: string }[] };
    return data.content?.[0]?.text?.trim() ?? '';
  }

  // openai
  const key = await getSecret('openaiKey');
  if (!key) throw new Error('OpenAI API key not set (Settings → AI).');
  const res = await fetch('https://api.openai.com/v1/chat/completions', {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${key}` },
    body: JSON.stringify({
      model,
      max_tokens: 200,
      messages: [
        {
          role: 'user',
          content: [
            { type: 'text', text: prompt },
            { type: 'image_url', image_url: { url: `data:image/jpeg;base64,${base64}` } },
          ],
        },
      ],
    }),
  });
  if (!res.ok) throw new Error(`OpenAI error ${res.status}: ${await res.text()}`);
  const data = (await res.json()) as { choices?: { message?: { content?: string } }[] };
  return data.choices?.[0]?.message?.content?.trim() ?? '';
}
