import { readFile, writeFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { join } from 'node:path';
import { getPaths } from './paths.ts';

/**
 * App settings (userData/settings.json). Holds MCP server config; the bearer token is
 * a random per-install secret (addendum §4). It never leaves the machine except when
 * the user pastes a snippet into an MCP client config.
 */
export interface McpSettings {
  enabled: boolean;
  port: number;
  token: string;
}

export interface Settings {
  mcp: McpSettings;
}

const DEFAULTS: Settings = {
  mcp: { enabled: false, port: 8629, token: randomBytes(24).toString('hex') },
};

let cache: Settings | null = null;

function settingsPath(): string {
  return join(getPaths().userData, 'settings.json');
}

export async function getSettings(): Promise<Settings> {
  if (cache) return cache;
  try {
    const raw = await readFile(settingsPath(), 'utf8');
    const parsed = JSON.parse(raw) as Partial<Settings>;
    cache = {
      mcp: { ...DEFAULTS.mcp, ...parsed.mcp },
    };
  } catch {
    cache = structuredClone(DEFAULTS);
  }
  return cache;
}

export async function saveSettings(patch: Partial<Settings>): Promise<Settings> {
  const current = await getSettings();
  cache = {
    mcp: { ...current.mcp, ...patch.mcp },
  };
  await writeFile(settingsPath(), JSON.stringify(cache, null, 2), 'utf8');
  return cache;
}

export async function rotateMcpToken(): Promise<string> {
  const token = randomBytes(24).toString('hex');
  await saveSettings({ mcp: { ...(await getSettings()).mcp, token } });
  return token;
}
