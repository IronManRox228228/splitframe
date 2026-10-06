import { describe, expect, it } from 'vitest';
import { MOTION_SANDBOX_CSP, buildCsp } from './csp.ts';

describe('buildCsp', () => {
  it('is strict in production: no inline script, no websocket or network connections', () => {
    const csp = buildCsp({ dev: false });
    expect(csp).toContain("script-src 'self';");
    expect(csp).toContain("connect-src 'self';");
    expect(csp).not.toMatch(/\bws:/);
    expect(csp).toContain("object-src 'none'");
    expect(csp).toContain("base-uri 'none'");
    expect(csp).toContain('frame-src cbsandbox:');
  });

  it('only loosens script and loopback websocket sources in dev', () => {
    const csp = buildCsp({ dev: true });
    expect(csp).toContain("script-src 'self' 'unsafe-inline'");
    expect(csp).toContain('ws://localhost:*');
    expect(csp).not.toMatch(/(^|[ ;])ws:[ ;]/);
  });
});

describe('MOTION_SANDBOX_CSP', () => {
  it('allows code evaluation but nothing else (no connect-src fallback)', () => {
    expect(MOTION_SANDBOX_CSP).toContain("default-src 'none'");
    expect(MOTION_SANDBOX_CSP).toContain("'unsafe-eval'");
    expect(MOTION_SANDBOX_CSP).not.toContain('connect-src');
  });
});
