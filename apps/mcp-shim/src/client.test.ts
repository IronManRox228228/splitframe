import { describe, expect, it } from 'vitest';
import { McpHttpClient, parseSse, type FetchLike } from './client.ts';

type Call = { method: string; headers: Record<string, string>; body?: string };

function fakeServer(handler: (call: Call, n: number) => { status?: number; headers?: Record<string, string>; body?: string }) {
  const calls: Call[] = [];
  const fetch: FetchLike = async (_url, init) => {
    calls.push(init);
    const r = handler(init, calls.length);
    const headers = r.headers ?? {};
    return {
      ok: (r.status ?? 200) < 400,
      status: r.status ?? 200,
      headers: { get: (name: string) => headers[name.toLowerCase()] ?? null },
      text: async () => r.body ?? '',
    };
  };
  return { fetch, calls };
}

describe('McpHttpClient', () => {
  it('captures the session id from initialize and replays it on later requests', async () => {
    const { fetch, calls } = fakeServer((_c, n) =>
      n === 1
        ? { headers: { 'mcp-session-id': 'sess-1', 'content-type': 'application/json' }, body: '{"jsonrpc":"2.0","id":1,"result":{}}' }
        : { headers: { 'content-type': 'application/json' }, body: '{"jsonrpc":"2.0","id":2,"result":{"tools":[]}}' },
    );
    const client = new McpHttpClient({ url: 'http://x/mcp', token: 'tok', fetch });
    await client.send({ jsonrpc: '2.0', id: 1, method: 'initialize' });
    const out = await client.send({ jsonrpc: '2.0', id: 2, method: 'tools/list' });
    expect(calls[0]!.headers['mcp-session-id']).toBeUndefined();
    expect(calls[1]!.headers['mcp-session-id']).toBe('sess-1');
    expect(calls[1]!.headers['authorization']).toBe('Bearer tok');
    expect(out).toEqual(['{"jsonrpc":"2.0","id":2,"result":{"tools":[]}}']);
  });

  it('writes nothing for notifications answered with an empty 202', async () => {
    const { fetch } = fakeServer(() => ({ status: 202 }));
    const client = new McpHttpClient({ url: 'http://x/mcp', fetch });
    expect(await client.send({ jsonrpc: '2.0', method: 'notifications/initialized' })).toEqual([]);
  });

  it('turns HTTP failures and unreachable servers into JSON-RPC errors for requests', async () => {
    const bad = fakeServer(() => ({ status: 401, body: 'Unauthorized' }));
    const out = await new McpHttpClient({ url: 'u', fetch: bad.fetch }).send({ jsonrpc: '2.0', id: 7, method: 'tools/list' });
    expect(JSON.parse(out[0]!)).toMatchObject({ id: 7, error: { message: expect.stringContaining('401') } });

    const down: FetchLike = async () => {
      throw new Error('ECONNREFUSED');
    };
    const out2 = await new McpHttpClient({ url: 'u', fetch: down }).send({ jsonrpc: '2.0', id: 8, method: 'tools/list' });
    expect(JSON.parse(out2[0]!)).toMatchObject({ id: 8, error: { message: expect.stringContaining('ECONNREFUSED') } });
    expect(await new McpHttpClient({ url: 'u', fetch: down }).send({ jsonrpc: '2.0', method: 'notifications/x' })).toEqual([]);
  });

  it('forgets the session when the server no longer knows it', async () => {
    const { fetch } = fakeServer((_c, n) => (n === 1 ? { headers: { 'mcp-session-id': 's' }, body: '{}' } : { status: 404, body: 'Unknown session' }));
    const client = new McpHttpClient({ url: 'u', fetch });
    await client.send({ jsonrpc: '2.0', id: 1, method: 'initialize' });
    await client.send({ jsonrpc: '2.0', id: 2, method: 'tools/list' });
    expect(client.session).toBeNull();
  });

  it('unwraps server-sent-event responses', async () => {
    const { fetch } = fakeServer(() => ({ headers: { 'content-type': 'text/event-stream' }, body: 'event: message\ndata: {"id":1}\n\n' }));
    expect(await new McpHttpClient({ url: 'u', fetch }).send({ jsonrpc: '2.0', id: 1, method: 'ping' })).toEqual(['{"id":1}']);
    expect(parseSse('data: a\n\ndata: b\n\n')).toEqual(['a', 'b']);
  });
});
