import { describe, expect, it } from 'vitest';
import { buildMotionSandboxHtml } from './motion-sandbox.ts';

describe('buildMotionSandboxHtml', () => {
  it('embeds the runtime and the message handler', () => {
    const html = buildMotionSandboxHtml('const evaluateFrame = () => 1;');
    expect(html).toContain('const evaluateFrame = () => 1;');
    expect(html).toContain("addEventListener('message'");
  });

  it('cannot be broken out of by a closing script tag inside the runtime', () => {
    const html = buildMotionSandboxHtml('const s = "</script><b>x</b>";');
    expect(html.match(/<\/script>/g)).toHaveLength(2); // only our own two
  });

  it('ships the real runtime by default', () => {
    const html = buildMotionSandboxHtml();
    expect(html).toContain('evaluateFrame');
    expect(html).toContain('useCurrentFrame');
  });
});
