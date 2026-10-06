import { describe, expect, it } from 'vitest';
import { formatTimecode, framesToMs, msToFrames } from './time.ts';

describe('formatTimecode', () => {
  it('formats minutes, seconds and tenths', () => {
    expect(formatTimecode(0, 30)).toBe('00:00.0');
    expect(formatTimecode(216, 30)).toBe('00:07.2');
  });

  it('carries into the next minute instead of printing 60 seconds', () => {
    expect(formatTimecode(Math.round(59.96 * 30), 30)).toBe('01:00.0');
  });

  it('adds hours once the timeline passes an hour', () => {
    expect(formatTimecode(3723 * 30, 30)).toBe('01:02:03.0');
  });

  it('clamps negative frames to zero', () => {
    expect(formatTimecode(-5, 30)).toBe('00:00.0');
  });
});

describe('ms/frame conversion', () => {
  it('round-trips whole frames', () => {
    expect(msToFrames(framesToMs(90, 30), 30)).toBe(90);
  });
});
