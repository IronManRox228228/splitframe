import { describe, expect, it } from 'vitest';
import { aspectLabel, editedLabel, formatDuration, shortFfmpegVersion } from './format.ts';

describe('home formatting', () => {
  it('formats durations', () => {
    expect(formatDuration(0)).toBe('0:00');
    expect(formatDuration(11_000)).toBe('0:11');
    expect(formatDuration(272_000)).toBe('4:32');
    expect(formatDuration(3_725_000)).toBe('1:02:05');
  });
  it('names aspect ratios', () => {
    expect(aspectLabel(1080, 1920)).toBe('9:16');
    expect(aspectLabel(1920, 1080)).toBe('16:9');
    expect(aspectLabel(1080, 1080)).toBe('1:1');
    expect(aspectLabel(1080, 1350)).toBe('4:5');
  });
  it('words relative edit times', () => {
    const now = new Date(2026, 9, 7, 15, 0, 0);
    expect(editedLabel(new Date(2026, 9, 7, 14, 48, 0).toISOString(), now)).toBe('Edited 12 min ago');
    expect(editedLabel(new Date(2026, 9, 7, 12, 30, 0).toISOString(), now)).toBe('Edited 2 h ago');
    expect(editedLabel(new Date(2026, 9, 6, 23, 0, 0).toISOString(), now)).toBe('Edited yesterday');
    expect(editedLabel(new Date(2026, 9, 2, 9, 0, 0).toISOString(), now)).toBe('Edited Oct 2');
    expect(editedLabel(new Date(2025, 11, 25, 9, 0, 0).toISOString(), now)).toBe('Edited Dec 25, 2025');
  });
  it('trims ffmpeg versions', () => {
    expect(shortFfmpegVersion('8.1-essentials_build-www.gyan.dev')).toBe('8.1');
    expect(shortFfmpegVersion('N-12345-gabc')).toBe('N-12345-gabc');
  });
});
