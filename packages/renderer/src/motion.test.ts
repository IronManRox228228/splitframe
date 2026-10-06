import { describe, expect, it } from 'vitest';
import { validateMotionCode } from './motion.ts';

describe('validateMotionCode', () => {
  it('accepts a normal composition', () => {
    const code = `(props) => { const f = useCurrentFrame(); return Box({ opacity: interpolate(f, [0, 10], [0, 1]), children: [Text({ text: String(props.title) })] }); }`;
    expect(validateMotionCode(code)).toEqual({ ok: true });
  });

  it('rejects network and DOM globals', () => {
    expect(validateMotionCode('() => fetch("https://example.com")').ok).toBe(false);
    expect(validateMotionCode('() => window.parent').ok).toBe(false);
  });

  it('rejects reaching the Function constructor through the prototype chain', () => {
    expect(validateMotionCode('() => [].constructor.constructor("return 1")()').ok).toBe(false);
    expect(validateMotionCode('() => ({})["constructor"]').ok).toBe(false);
    expect(validateMotionCode('() => (0).__proto__').ok).toBe(false);
    expect(validateMotionCode('() => Reflect.ownKeys({})').ok).toBe(false);
  });
});
