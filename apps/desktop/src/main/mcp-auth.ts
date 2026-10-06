import { timingSafeEqual } from 'node:crypto';

/** Request checks for the loopback MCP server, kept pure so they can be unit-tested. */

export function tokenMatches(authorization: string | undefined, token: string): boolean {
  const given = Buffer.from(authorization ?? '');
  const expected = Buffer.from(`Bearer ${token}`);
  return given.length === expected.length && timingSafeEqual(given, expected);
}

/** DNS-rebinding guard: only the exact loopback host:port the server listens on. */
export function hostAllowed(host: string | undefined, port: number): boolean {
  return host === `127.0.0.1:${port}` || host === `localhost:${port}`;
}

/** Browsers send an Origin; only loopback pages may call the server. Non-browser clients send none. */
export function originAllowed(origin: string | undefined): boolean {
  return !origin || /^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(origin);
}

/**
 * The command shown to users for MCP clients that launch a command (stdio). It runs the
 * bundled shim with the absolute path to its entry file, so nothing is fetched from npm.
 */
export function buildMcpSnippets(opts: { url: string; token: string; shimPath: string }) {
  const { url, token, shimPath } = opts;
  const env = { CUTBOARD_MCP_URL: url, CUTBOARD_MCP_TOKEN: token };
  return {
    url,
    claudeCode: `claude mcp add --transport http cutboard ${url} --header "Authorization: Bearer ${token}"`,
    claudeDesktop: JSON.stringify({ mcpServers: { cutboard: { command: 'node', args: [shimPath], env } } }, null, 2),
    // TOML literal strings ('...') need no escaping, which keeps Windows paths intact
    codex: `[mcp_servers.cutboard]\ncommand = "node"\nargs = ['${shimPath}']\nenv = { CUTBOARD_MCP_URL = "${url}", CUTBOARD_MCP_TOKEN = "${token}" }`,
    cursor: JSON.stringify({ mcpServers: { cutboard: { url, headers: { Authorization: `Bearer ${token}` } } } }, null, 2),
  };
}
