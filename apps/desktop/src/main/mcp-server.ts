import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { z } from 'zod';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { AGENT_SYSTEM_PROMPT } from '@cutboard/agent';
import { registry, callTool } from './tools-bridge.ts';
import { getSettings } from './settings.ts';
import { broadcast } from './events.ts';

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

  const server = createServer((req, res) => {
    void handle(req, res);
  });
  server.on('error', (err) => {
    process.stderr.write(`[mcp] server error: ${err.message}\n`);
    listening = false;
  });
  await new Promise<void>((resolve) => server.listen(mcp.port, '127.0.0.1', resolve));
  listening = true;
  httpServer = server;
  process.stderr.write(`[mcp] listening on 127.0.0.1:${mcp.port}\n`);
  return { port: mcp.port };
}

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
  const host = req.headers.host ?? '';
  if (!host.startsWith(`127.0.0.1:${mcp.port}`) && !host.startsWith(`localhost:${mcp.port}`)) {
    res.writeHead(403).end('Forbidden host');
    return;
  }
  const origin = req.headers.origin;
  if (origin && !/^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(origin)) {
    res.writeHead(403).end('Forbidden origin');
    return;
  }

  // bearer token
  const auth = req.headers.authorization ?? '';
  if (auth !== `Bearer ${mcp.token}`) {
    res.writeHead(401, { 'www-authenticate': 'Bearer realm="cutboard-mcp"' }).end('Unauthorized');
    return;
  }

  const clientId = String(req.headers['x-cutboard-client'] ?? 'unknown');
  const sessionId = req.headers['mcp-session-id'];

  if (req.method === 'POST') {
    let body = '';
    for await (const chunk of req) body += String(chunk);
    let parsed: unknown;
    try {
      parsed = JSON.parse(body);
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

    const transport = new StreamableHTTPServerTransport({
      sessionIdGenerator: () => `sess-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
      enableJsonResponse: true,
      onsessioninitialized: (id) => {
        transports.set(id, { transport, client: clientId });
        broadcast('event', { type: 'mcp:connected', payload: { sessionId: id, client: clientId } });
      },
    });
    transport.onclose = () => {
      const id = transport.sessionId;
      if (id) transports.delete(id);
    };
    const mcpServer = await buildServer(clientId);
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
