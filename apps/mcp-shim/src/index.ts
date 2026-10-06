#!/usr/bin/env node
/**
 * Cutboard MCP stdio shim (addendum §4): proxies MCP stdio ⇄ the desktop app's loopback
 * Streamable HTTP server. Many MCP clients (Claude Desktop, Cursor, Codex) prefer
 * launching a command; the app itself serves HTTP on 127.0.0.1 with a per-install token.
 *
 * Env: CUTBOARD_MCP_URL (default http://127.0.0.1:8629/mcp), CUTBOARD_MCP_TOKEN.
 * Run it with `node <path to dist/index.js>`; the app's MCP settings show the exact command.
 */
import { McpHttpClient } from './client.js';

const url = process.env['CUTBOARD_MCP_URL'] ?? 'http://127.0.0.1:8629/mcp';
const token = process.env['CUTBOARD_MCP_TOKEN'] ?? '';

process.stderr.write(`[cutboard-mcp] shim target: ${url}\n`);

const client = new McpHttpClient({ url, token });

// messages are forwarded strictly in order: `initialize` must finish (and yield the
// session id) before the requests that follow it are sent
let queue: Promise<void> = Promise.resolve();
let buffer = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk: string) => {
  buffer += chunk;
  let idx: number;
  while ((idx = buffer.indexOf('\n')) !== -1) {
    const line = buffer.slice(0, idx).trim();
    buffer = buffer.slice(idx + 1);
    if (!line) continue;
    queue = queue.then(async () => {
      try {
        for (const out of await client.send(JSON.parse(line))) process.stdout.write(out + '\n');
      } catch (err) {
        process.stderr.write(`[cutboard-mcp] error: ${err instanceof Error ? err.message : String(err)}\n`);
      }
    });
  }
});

process.stdin.on('end', () => {
  void queue.then(() => client.close()).finally(() => process.exit(0));
});
