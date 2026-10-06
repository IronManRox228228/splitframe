/**
 * Content-Security-Policy for the renderer windows. Shared by the main process (response
 * header) and the renderer build (`<meta http-equiv>`, which is what actually applies to
 * the packaged app's file:// pages), so both always agree.
 */
export function buildCsp(opts: { dev: boolean }): string {
  const { dev } = opts;
  return [
    "default-src 'self'",
    // dev needs the inline preamble vite's react plugin injects; production stays strict
    `script-src 'self'${dev ? " 'unsafe-inline'" : ''}`,
    "style-src 'self' 'unsafe-inline'",
    "img-src 'self' data: blob: cbmedia:",
    "media-src 'self' blob: cbmedia:",
    // dev: vite HMR over a loopback websocket; production: no network at all
    `connect-src 'self'${dev ? ' ws://localhost:* http://localhost:*' : ''}`,
    "font-src 'self' data:",
    "worker-src 'self' blob:",
    // motion-graphic code runs in an isolated, network-less document served from cbsandbox://
    'frame-src cbsandbox:',
    "object-src 'none'",
    "base-uri 'none'",
    "form-action 'none'",
  ].join('; ');
}

/**
 * Policy of the motion-graphic sandbox document. It may evaluate code (that is its job)
 * but has no network, no frames and no other resources, so generated code cannot reach
 * anything outside the scene tree it returns.
 */
export const MOTION_SANDBOX_CSP = "default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval'";
