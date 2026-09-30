#!/usr/bin/env node
/**
 * Cutboard MCP stdio shim (addendum §4): proxies MCP stdio ⇄ the desktop app's loopback
 * Streamable HTTP server. Many MCP clients (Claude Desktop, Cursor, Codex) prefer
 * launching a command; the app itself serves HTTP on 127.0.0.1 with a per-install token.
 *
 * Env: CUTBOARD_MCP_URL (default http://127.0.0.1:8629/mcp), CUTBOARD_MCP_TOKEN.
 *
 * Milestone 4 wires this into the app's MCP server; the transport logic lands there.
 * Registered as `cutboard-mcp` so clients can run `npx cutboard-mcp`.
 */
const url = process.env['CUTBOARD_MCP_URL'] ?? 'http://127.0.0.1:8629/mcp';
const token = process.env['CUTBOARD_MCP_TOKEN'] ?? '';

process.stderr.write(`[cutboard-mcp] shim target: ${url}\n`);

async function post(body: unknown): Promise<string> {
  const res = await fetch(url, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
      ...(token ? { authorization: `Bearer ${token}` } : {}),
    },
    body: JSON.stringify(body),
  });
  return res.text();
}

let buffer = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk: string) => {
  buffer += chunk;
  let idx: number;
  while ((idx = buffer.indexOf('\n')) !== -1) {
    const line = buffer.slice(0, idx).trim();
    buffer = buffer.slice(idx + 1);
    if (!line) continue;
    void (async () => {
      try {
        const message = JSON.parse(line);
        const text = await post(message);
        if (text) process.stdout.write(text + '\n');
      } catch (err) {
        process.stderr.write(`[cutboard-mcp] error: ${err instanceof Error ? err.message : String(err)}\n`);
      }
    })();
  }
});

process.stdin.on('end', () => process.exit(0));
