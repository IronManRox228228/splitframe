import { describe, expect, it } from 'vitest';
import { timestampAccuracyFlags } from './whisper-flags.ts';

describe('timestampAccuracyFlags', () => {
  it('turns flash attention off when the build supports it', () => {
    const help = '  -fa,       --flash-attn           [true   ] enable flash attention\n  -nfa,      --no-flash-attn        [false  ] disable flash attention';
    expect(timestampAccuracyFlags(help)).toEqual(['-nfa']);
  });

  it('passes nothing to older builds that would reject the flag', () => {
    expect(timestampAccuracyFlags('  -fa,       --flash-attn           [false  ] flash attention')).toEqual([]);
    expect(timestampAccuracyFlags('')).toEqual([]);
  });
});
