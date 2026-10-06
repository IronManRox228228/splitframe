import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { z } from 'zod';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { isInitializeRequest } from '@modelcontextprotocol/sdk/types.js';
import { AGENT_SYSTEM_PROMPT } from '@cutboard/agent';
import { registry, callTool } from './tools-bridge.ts';
import { getSettings } from './settings.ts';
import { broadcast } from './events.ts';
import { hostAllowed, originAllowed, tokenMatches } from './mcp-auth.ts';

/** Largest JSON-RPC body accepted (tool arguments are small; this only guards runaway clients). */
const MAX_BODY_BYTES = 4 * 1024 * 1024;

/**
 * Local MCP server (addendum §4): Streamable HTTP on 127.0.0.1 only, per-install bearer
 * token, Origin/Host validation (DNS-rebinding protection). Remote tunnels are
 * explicitly NOT supported in v1 (docs/DECISIONS.md #7).
 *
 * Stateful per-session transports keyed by the MCP session id, so clients keep their
 * session across requests while revocation/rotation kills them instantly.
 */

interface McpActivityEntry {
  at: string;
  client: string;
  tool: string;
  ok: boolean;
  ms: number;
}
const activity: McpActivityEntry[] = [];
const transports = new Map<string, { transport: StreamableHTTPServerTransport; client: string }>();

function logActivity(entry: McpActivityEntry): void {
  activity.unshift(entry);
  if (activity.length > 200) activity.pop();
  broadcast('event', { type: 'mcp:activity', payload: entry });
}

async function buildServer(clientName: string): Promise<McpServer> {
  const server = new McpServer(
    { name: 'cutboard', version: '0.1.0' },
    { instructions: AGENT_SYSTEM_PROMPT },
  );
  for (const tool of registry.list()) {
    const inputShape: z.ZodRawShape =
      'shape' in tool.input ? ((tool.input as z.ZodObject).shape as z.ZodRawShape) : {};
    server.tool(tool.name, tool.description, inputShape, async (args: unknown) => {
      const t0 = Date.now();
      try {
        const result = await callTool(tool.name, args, `mcp:${clientName}` as never);
        logActivity({ at: new Date().toISOString(), client: clientName, tool: tool.name, ok: true, ms: Date.now() - t0 });
        return { content: [{ type: 'text' as const, text: JSON.stringify(result, null, 1) }] };
      } catch (err) {
        logActivity({ at: new Date().toISOString(), client: clientName, tool: tool.name, ok: false, ms: Date.now() - t0 });
        const message = err instanceof Error ? err.message : String(err);
        const hint = (err as { hint?: string }).hint;
        return {
          isError: true,
          content: [{ type: 'text' as const, text: hint ? `${message}\n\nHint: ${hint}` : message }],
        };
      }
    });
  }
  return server;
}

export async function startMcpServer(): Promise<{ port: number } | { error: string }> {
  const { mcp } = await getSettings();
  if (!mcp.enabled) return { error: 'MCP server is disabled in settings' };
  if (listening) return { port: mcp.port };
  if (starting) return starting; // concurrent callers share one listen attempt

  starting = (async () => {
    const server = createServer((req, res) => {
      handle(req, res).catch((err) => {
        process.stderr.write(`[mcp] request failed: ${err instanceof Error ? err.message : String(err)}\n`);
        if (!res.headersSent) res.writeHead(500, { 'content-type': 'text/plain' });
        res.end('Internal error');
      });
    });
    try {
      // an unavailable port (EADDRINUSE, EACCES) must reject instead of leaving the promise pending
      await new Promise<void>((resolve, reject) => {
        server.once('error', reject);
        server.listen(mcp.port, '127.0.0.1', () => {
          server.off('error', reject);
          resolve();
        });
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      process.stderr.write(`[mcp] could not listen on 127.0.0.1:${mcp.port}: ${message}\n`);
      return { error: `Could not start the MCP server on port ${mcp.port}: ${message}` };
    }
    server.on('error', (err) => {
      process.stderr.write(`[mcp] server error: ${err.message}\n`);
    });
    listening = true;
    httpServer = server;
    process.stderr.write(`[mcp] listening on 127.0.0.1:${mcp.port}\n`);
    return { port: mcp.port };
  })().finally(() => {
    starting = null;
  });
  return starting;
}

let starting: Promise<{ port: number } | { error: string }> | null = null;

let listening = false;
let httpServer: ReturnType<typeof createServer> | null = null;

export function stopMcpServer(): void {
  httpServer?.close();
  httpServer = null;
  listening = false;
  for (const [id, t] of transports) {
    void t.transport.close();
    transports.delete(id);
  }
}

export function revokeMcpSessions(): void {
  // token rotation invalidates every existing session
  for (const [id, t] of transports) {
    void t.transport.close();
    transports.delete(id);
  }
}

export function getMcpActivity(): McpActivityEntry[] {
  return activity;
}

async function handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
  const { mcp } = await getSettings();

  // loopback-only + DNS-rebinding protection (addendum §4)
  if (!hostAllowed(req.headers.host, mcp.port)) {
    res.writeHead(403).end('Forbidden host');
    return;
  }
  if (!originAllowed(req.headers.origin)) {
    res.writeHead(403).end('Forbidden origin');
    return;
  }

  // bearer token (constant-time compare)
  if (!tokenMatches(req.headers.authorization, mcp.token)) {
    res.writeHead(401, { 'www-authenticate': 'Bearer realm="cutboard-mcp"' }).end('Unauthorized');
    return;
  }

  const clientId = String(req.headers['x-cutboard-client'] ?? 'unknown');
  const sessionId = req.headers['mcp-session-id'];

  if (req.method === 'POST') {
    // collect raw bytes and decode once: decoding per chunk splits multi-byte characters
    const chunks: Buffer[] = [];
    let size = 0;
    for await (const chunk of req) {
      size += (chunk as Buffer).length;
      if (size > MAX_BODY_BYTES) {
        res.writeHead(413).end('Request too large');
        return;
      }
      chunks.push(chunk as Buffer);
    }
    let parsed: unknown;
    try {
      parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    } catch {
      res.writeHead(400).end('Invalid JSON');
      return;
    }

    // reuse an existing transport when the client presents a session id
    if (typeof sessionId === 'string' && transports.has(sessionId)) {
      const entry = transports.get(sessionId)!;
      await entry.transport.handleRequest(req, res, parsed);
      return;
    }

    // only `initialize` may open a session; any other request without a live session id used
    // to spin up a fresh, never-initialised server per request
    if (!isInitializeRequest(parsed)) {
      res
        .writeHead(404, { 'content-type': 'application/json' })
        .end(JSON.stringify({ jsonrpc: '2.0', error: { code: -32001, message: 'Unknown or missing MCP session. Send initialize first.' }, id: null }));
      return;
    }

    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: () => `sess-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      enableJsonResponse: true,
      onsessioninitialized: (id) => {
        transports.set(id, { transport, client: clientId });
        broadcast('event', { type: 'mcp:connected', payload: { sessionId: id, client: clientId } });
      },
    });
    const mcpServer = await buildServer(clientId);
    transport.onclose = () => {
      const id = transport.sessionId;
      if (id) transports.delete(id);
    };
    await mcpServer.connect(transport);
    await transport.handleRequest(req, res, parsed);
    return;
  }

  if (req.method === 'GET' || req.method === 'DELETE') {
    if (typeof sessionId !== 'string' || !transports.has(sessionId)) {
      res.writeHead(404).end('Unknown session');
      return;
    }
    const entry = transports.get(sessionId)!;
    await entry.transport.handleRequest(req, res);
    return;
  }

  res.writeHead(405).end('Method not allowed');
}
