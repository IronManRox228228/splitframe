/**
 * Streamable-HTTP client used by the stdio shim. The desktop app's MCP server is
 * stateful: `initialize` returns an `mcp-session-id` header that every later request must
 * carry, so the client remembers it and replays it.
 */

export type FetchLike = (url: string, init: { method: string; headers: Record<string, string>; body?: string }) => Promise<{
  ok: boolean;
  status: number;
  headers: { get(name: string): string | null };
  text(): Promise<string>;
}>;

export interface ShimOptions {
  url: string;
  token?: string;
  fetch?: FetchLike;
}

interface JsonRpcMessage {
  jsonrpc?: string;
  id?: string | number | null;
  method?: string;
}

/** `data:` payloads of a text/event-stream body (one JSON-RPC message each). */
export function parseSse(body: string): string[] {
  const messages: string[] = [];
  for (const event of body.split(/\r?\n\r?\n/)) {
    const data = event
      .split(/\r?\n/)
      .filter((line) => line.startsWith('data:'))
      .map((line) => line.slice(5).trimStart())
      .join('\n');
    if (data) messages.push(data);
  }
  return messages;
}

export class McpHttpClient {
  private sessionId: string | null = null;
  private readonly fetchImpl: FetchLike;

  constructor(private readonly opts: ShimOptions) {
    this.fetchImpl = opts.fetch ?? ((url, init) => fetch(url, init));
  }

  get session(): string | null {
    return this.sessionId;
  }

  private headers(): Record<string, string> {
    return {
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
      ...(this.opts.token ? { authorization: `Bearer ${this.opts.token}` } : {}),
      ...(this.sessionId ? { 'mcp-session-id': this.sessionId } : {}),
    };
  }

  /**
   * Forward one JSON-RPC message. Returns the message(s) to write to stdout (none for
   * notifications). Transport failures on a request become JSON-RPC errors so the MCP
   * client never waits forever for a reply.
   */
  async send(message: JsonRpcMessage): Promise<string[]> {
    const isRequest = message.id !== undefined && message.method !== undefined;
    try {
      const res = await this.fetchImpl(this.opts.url, { method: 'POST', headers: this.headers(), body: JSON.stringify(message) });
      const sid = res.headers.get('mcp-session-id');
      if (sid) this.sessionId = sid;
      const text = await res.text();
      if (!res.ok) {
        if (res.status === 404) this.sessionId = null; // server restarted: the session is gone
        return isRequest ? [rpcError(message.id ?? null, `HTTP ${res.status}: ${text.slice(0, 200)}`)] : [];
      }
      if (!text.trim()) return [];
      const type = res.headers.get('content-type') ?? '';
      return type.includes('text/event-stream') ? parseSse(text) : [text.trim()];
    } catch (err) {
      const reason = err instanceof Error ? err.message : String(err);
      return isRequest ? [rpcError(message.id ?? null, `Cutboard is not reachable (${reason}). Is the app running with the MCP server enabled?`)] : [];
    }
  }

  /** Best-effort session teardown when stdin closes. */
  async close(): Promise<void> {
    if (!this.sessionId) return;
    try {
      await this.fetchImpl(this.opts.url, { method: 'DELETE', headers: this.headers() });
    } catch {
      /* the app may already be gone */
    }
    this.sessionId = null;
  }
}

function rpcError(id: string | number | null, message: string): string {
  return JSON.stringify({ jsonrpc: '2.0', id, error: { code: -32000, message } });
}
