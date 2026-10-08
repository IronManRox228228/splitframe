import { describe, expect, it } from 'vitest';
import { silenceIntervalsFromStderr } from './silence.ts';

describe('silenceIntervalsFromStderr', () => {
  it('reads silencedetect output as ms intervals and closes an open one at the end', () => {
    const stderr = [
      '[silencedetect @ 0x1] silence_start: 2.986',
      '[silencedetect @ 0x1] silence_end: 4.586 | silence_duration: 1.6',
      '[silencedetect @ 0x1] silence_start: 58.9',
    ].join('\n');
    expect(silenceIntervalsFromStderr(stderr, 60000)).toEqual([
      { startMs: 2986, endMs: 4586 },
      { startMs: 58900, endMs: 60000 },
    ]);
  });
  it('returns nothing for audio without silence', () => {
    expect(silenceIntervalsFromStderr('size=N/A', 5000)).toEqual([]);
  });
});
