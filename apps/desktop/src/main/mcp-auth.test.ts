import { describe, expect, it } from 'vitest';
import { buildMcpSnippets, hostAllowed, originAllowed, tokenMatches } from './mcp-auth.ts';

describe('tokenMatches', () => {
  it('accepts only the exact bearer token', () => {
    expect(tokenMatches('Bearer abc123', 'abc123')).toBe(true);
    expect(tokenMatches('Bearer abc124', 'abc123')).toBe(false);
    expect(tokenMatches('Bearer abc1234', 'abc123')).toBe(false);
    expect(tokenMatches(undefined, 'abc123')).toBe(false);
    expect(tokenMatches('abc123', 'abc123')).toBe(false);
  });
});

describe('hostAllowed', () => {
  it('requires the exact loopback host and port', () => {
    expect(hostAllowed('127.0.0.1:8629', 8629)).toBe(true);
    expect(hostAllowed('localhost:8629', 8629)).toBe(true);
    expect(hostAllowed('127.0.0.1:86290', 8629)).toBe(false);
    expect(hostAllowed('127.0.0.1:8629.evil.com', 8629)).toBe(false);
    expect(hostAllowed(undefined, 8629)).toBe(false);
  });
});

describe('originAllowed', () => {
  it('allows no origin and loopback origins only', () => {
    expect(originAllowed(undefined)).toBe(true);
    expect(originAllowed('http://localhost:5173')).toBe(true);
    expect(originAllowed('https://evil.example')).toBe(false);
    expect(originAllowed('http://127.0.0.1.evil.example')).toBe(false);
  });
});

describe('buildMcpSnippets', () => {
  const shimPath = 'C:\\Program Files\\Cutboard\\resources\\mcp-shim\\index.js';
  const snippets = buildMcpSnippets({ url: 'http://127.0.0.1:8629/mcp', token: 'tok', shimPath });

  it('launches the shim by absolute path with node, never through npx', () => {
    const desktop = JSON.parse(snippets.claudeDesktop).mcpServers.cutboard;
    expect(desktop.command).toBe('node');
    expect(desktop.args).toEqual([shimPath]);
    expect(desktop.env.CUTBOARD_MCP_TOKEN).toBe('tok');
    for (const text of Object.values(snippets)) expect(text).not.toContain('npx');
  });

  it('keeps Windows backslashes intact in the TOML snippet', () => {
    expect(snippets.codex).toContain(`args = ['${shimPath}']`);
  });
});
