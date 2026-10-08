import { describe, expect, it } from 'vitest';
import { aspectLabel, fmtDur, fmtTime, parseTime, toFrames, toSec } from './units.ts';

describe('units', () => {
  it('converts seconds and frames', () => {
    expect(toFrames(4, 30)).toBe(120);
    expect(toFrames(1.5, 24)).toBe(36);
    expect(toSec(90, 30)).toBe(3);
  });
  it('parses timecodes', () => {
    expect(parseTime(7.5)).toBe(7.5);
    expect(parseTime('7.5')).toBe(7.5);
    expect(parseTime('0:07')).toBe(7);
    expect(parseTime('1:02.5')).toBe(62.5);
    expect(parseTime('1:00:03')).toBe(3603);
    expect(parseTime('1m30s')).toBe(90);
    expect(parseTime('90 seconds')).toBe(90);
    expect(parseTime('half a minute')).toBeNull();
    expect(parseTime(-1)).toBeNull();
    expect(parseTime('')).toBeNull();
  });
  it('formats', () => {
    expect(fmtTime(7)).toBe('0:07');
    expect(fmtTime(12.46)).toBe('0:12.5');
    expect(fmtTime(59.97)).toBe('1:00');
    expect(fmtTime(3723)).toBe('1:02:03');
    expect(fmtDur(5)).toBe('5s');
    expect(fmtDur(5.44)).toBe('5.4s');
    expect(fmtDur(72)).toBe('1m12s');
    expect(aspectLabel(1080, 1920)).toBe('9:16');
    expect(aspectLabel(1080, 1350)).toBe('4:5');
  });
});
