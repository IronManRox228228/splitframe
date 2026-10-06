import { readFile, writeFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { app, safeStorage } from 'electron';
import { getPaths } from './paths.ts';

/**
 * App settings (userData/settings.json). The MCP bearer token is a random per-install
 * secret; provider API keys are stored encrypted with Electron safeStorage (OS
 * keychain-backed), never in plain files (addendum §3).
 */
export interface McpSettings {
  enabled: boolean;
  port: number;
  token: string;
}

export type VlmProvider = 'none' | 'ollama' | 'anthropic' | 'openai';

export interface AiSettings {
  /** chat model provider for the built-in agent */
  agentProvider?: 'anthropic' | 'openai' | 'google' | 'openrouter' | 'ollama';
  agentModel?: string;
  agentKeyEnc?: string;
  /** scene-description VLM */
  vlmProvider?: VlmProvider;
  vlmModel?: string;
  ollamaUrl?: string;
  anthropicKeyEnc?: string;
  openaiKeyEnc?: string;
}

export interface Settings {
  mcp: McpSettings;
  ai?: AiSettings;
  asr?: { model?: string };
}

const DEFAULTS: Settings = {
  mcp: { enabled: false, port: 8629, token: randomBytes(24).toString('hex') },
  ai: { vlmProvider: 'none' },
  asr: { model: 'base.en' },
};

let cache: Settings | null = null;

function settingsPath(): string {
  return join(getPaths().userData, 'settings.json');
}

export async function getSettings(): Promise<Settings> {
  if (cache) return cache;
  let loaded: Settings;
  try {
    const raw = await readFile(settingsPath(), 'utf8');
    const parsed = JSON.parse(raw) as Partial<Settings>;
    loaded = {
      mcp: { ...DEFAULTS.mcp, ...parsed.mcp },
      ai: { ...DEFAULTS.ai, ...parsed.ai },
      asr: { ...DEFAULTS.asr, ...parsed.asr },
    };
  } catch {
    loaded = structuredClone(DEFAULTS);
  }
  cache = loaded;
  return loaded;
}

export async function saveSettings(patch: Partial<Settings>): Promise<Settings> {
  const current = await getSettings();
  cache = {
    mcp: { ...current.mcp, ...patch.mcp ?? current.mcp },
    ai: { ...current.ai, ...patch.ai ?? {} },
    asr: { ...current.asr ?? DEFAULTS.asr, ...patch.asr ?? {} },
  };
  await writeFile(settingsPath(), JSON.stringify(cache, null, 2), 'utf8');
  return cache;
}

export async function rotateMcpToken(): Promise<string> {
  const token = randomBytes(24).toString('hex');
  await saveSettings({ mcp: { ...(await getSettings()).mcp, token } });
  return token;
}

function encPath(): string {
  return join(getPaths().userData, 'secrets.bin');
}

/** Encrypt one secret to disk via safeStorage; returns its storage handle name. */
export async function saveSecret(name: string, plain: string): Promise<void> {
  if (!safeStorage.isEncryptionAvailable()) throw new Error('OS encryption unavailable; cannot store keys.');
  const store = await readSecretStore();
  store[name] = safeStorage.encryptString(plain).toString('base64');
  await writeFile(encPath(), JSON.stringify(store), 'utf8');
}

export async function getSecret(name: string): Promise<string | null> {
  const store = await readSecretStore();
  const enc = store[name];
  if (!enc) return null;
  return safeStorage.decryptString(Buffer.from(enc, 'base64'));
}

export async function deleteSecret(name: string): Promise<void> {
  const store = await readSecretStore();
  if (!(name in store)) return;
  delete store[name];
  await writeFile(encPath(), JSON.stringify(store), 'utf8');
}

export async function getDecryptedKey(name: string): Promise<string | null> {
  return getSecret(name);
}

/**
 * Cloud agent keys are stored per provider so a key typed for one vendor is never sent
 * to another when the user switches providers (finding M8).
 */
export function agentKeySlot(provider: string): string {
  return `agentKey:${provider}`;
}

/**
 * Older builds kept a single shared `agentKey`. Adopt it for the provider that is
 * currently selected (the one it was entered for) and drop the shared slot. Runs before
 * a provider switch is applied, so the key can never follow the user to another vendor.
 */
export async function migrateLegacyAgentKey(): Promise<void> {
  const legacy = await getSecret('agentKey');
  if (!legacy) return;
  const selected = (await getSettings()).ai?.agentProvider ?? 'anthropic';
  if (!(await getSecret(agentKeySlot(selected)))) await saveSecret(agentKeySlot(selected), legacy);
  await deleteSecret('agentKey');
}

export async function getAgentKey(provider: string): Promise<string | null> {
  await migrateLegacyAgentKey();
  return getSecret(agentKeySlot(provider));
}

async function readSecretStore(): Promise<Record<string, string>> {
  try {
    return JSON.parse(await readFile(encPath(), 'utf8')) as Record<string, string>;
  } catch {
    return {};
  }
}

export function secretDir(): string {
  return app.getPath('userData');
}
