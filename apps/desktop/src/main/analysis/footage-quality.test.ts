import { describe, expect, it } from 'vitest';
import {
  aggregateSegment,
  detectIssues,
  overlapRatio,
  planSegments,
  qualityScore,
  sharpnessFromBlur,
  shakiness,
  type SegmentMetrics,
} from './footage-quality.ts';

const good: SegmentMetrics = {
  brightness: 0.45,
  contrast: 0.7,
  sharpness: 0.9,
  motion: 2,
  shakiness: 0,
  blackRatio: 0,
  frozenRatio: 0,
  audio: { loudnessLufs: -20, silenceRatio: 0 },
};

describe('planSegments', () => {
  it('keeps short scenes whole', () => {
    expect(planSegments([{ startMs: 0, endMs: 5000 }, { startMs: 5000, endMs: 35000 }])).toEqual([
      { sceneIndex: 0, windowIndex: null, startMs: 0, endMs: 5000 },
      { sceneIndex: 1, windowIndex: null, startMs: 5000, endMs: 35000 },
    ]);
  });

  it('splits a long scene into fixed windows', () => {
    const w = planSegments([{ startMs: 1000, endMs: 36000 }]);
    expect(w.map((s) => [s.windowIndex, s.startMs, s.endMs])).toEqual([
      [0, 1000, 11000],
      [1, 11000, 21000],
      [2, 21000, 31000],
      [3, 31000, 36000],
    ]);
  });

  it('absorbs a sliver at the end into the last window', () => {
    const w = planSegments([{ startMs: 0, endMs: 41000 }]);
    expect(w).toHaveLength(4);
    expect(w[3]).toMatchObject({ startMs: 30000, endMs: 41000 });
  });
});

describe('overlapRatio', () => {
  it('measures the covered fraction and merges overlapping intervals', () => {
    expect(overlapRatio([{ startSec: 0, endSec: 2 }], 1, 5)).toBeCloseTo(0.25);
    expect(
      overlapRatio([{ startSec: 1, endSec: 3 }, { startSec: 2, endSec: 4 }], 0, 10),
    ).toBeCloseTo(0.3);
    expect(overlapRatio([], 0, 10)).toBe(0);
    expect(overlapRatio([{ startSec: 0, endSec: 1 }], 5, 5)).toBe(0);
  });
});

describe('sharpness and shakiness', () => {
  it('maps blurdetect values calibrated on the bundled build', () => {
    expect(sharpnessFromBlur(3.96)).toBeGreaterThan(0.8); // crisp
    expect(sharpnessFromBlur(5.7)).toBeCloseTo(0.55, 1);
    expect(sharpnessFromBlur(8.6)).toBeLessThan(0.1);
    expect(sharpnessFromBlur(1)).toBe(1);
    expect(sharpnessFromBlur(20)).toBe(0);
  });

  it('uses the median so a single burst is not shake', () => {
    expect(shakiness([0, 0, 0, 40, 0])).toBe(0);
    expect(shakiness([20, 22, 21, 19])).toBe(1);
    expect(shakiness([])).toBe(0);
  });
});

describe('aggregateSegment', () => {
  const video = Array.from({ length: 8 }, (_, i) => ({
    t: i * 0.25,
    yAvg: 126,
    yLow: 41,
    yHigh: 210,
    blur: 4,
    ti: 0.1,
  }));

  it('reads only samples inside the segment', () => {
    const m = aggregateSegment({
      startMs: 1600,
      endMs: 1700,
      video: [...video, { t: 1.6, yAvg: 235, yLow: 200, yHigh: 235, blur: 4, ti: 0.1 }],
      black: [],
      frozen: [],
    });
    expect(m.brightness).toBeGreaterThan(0.8);
    expect(m.audio).toBeNull();
  });

  it('combines loudness, silence and black/frozen coverage', () => {
    const m = aggregateSegment({
      startMs: 0,
      endMs: 2000,
      video,
      black: [{ startSec: 0, endSec: 1 }],
      frozen: [{ startSec: 1, endSec: 1.5 }],
      loudness: [
        { t: 0, m: -120 },
        { t: 0.5, m: -20 },
        { t: 1, m: -20 },
        { t: 1.5, m: -120 },
      ],
      silence: [{ startSec: 1.5, endSec: 2 }],
    });
    expect(m.blackRatio).toBeCloseTo(0.5);
    expect(m.frozenRatio).toBeCloseTo(0.25);
    expect(m.audio?.loudnessLufs).toBeCloseTo(-20, 0);
    expect(m.audio?.silenceRatio).toBeCloseTo(0.25);
    expect(m.sharpness).toBeGreaterThan(0.8);
    expect(m.contrast).toBeCloseTo(169 / 219, 2);
  });
});

describe('qualityScore', () => {
  it('rates a sharp, well-lit, steady, audible take highly', () => {
    expect(qualityScore(good)).toBeGreaterThanOrEqual(90);
  });

  it('orders takes sensibly', () => {
    const blurry = qualityScore({ ...good, sharpness: 0.1 });
    const dark = qualityScore({ ...good, brightness: 0.05, contrast: 0.1 });
    const shaky = qualityScore({ ...good, shakiness: 1 });
    const mute = qualityScore({ ...good, audio: { loudnessLufs: -20, silenceRatio: 1 } });
    for (const worse of [blurry, dark, shaky, mute]) expect(worse).toBeLessThan(qualityScore(good));
    expect(blurry).toBeLessThan(mute); // sharpness weighs more than audio
  });

  it('redistributes the audio weight when there is no audio track', () => {
    expect(qualityScore({ ...good, audio: null })).toBeGreaterThanOrEqual(90);
  });

  it('black and frozen footage scores near zero', () => {
    expect(qualityScore({ ...good, blackRatio: 1 })).toBe(0);
    expect(qualityScore({ ...good, frozenRatio: 1 })).toBeLessThan(qualityScore(good) * 0.3);
  });

  it('stays within 0..100', () => {
    const worst = qualityScore({
      brightness: 1,
      contrast: 0,
      sharpness: 0,
      motion: 99,
      shakiness: 1,
      blackRatio: 0,
      frozenRatio: 0,
      audio: { loudnessLufs: null, silenceRatio: 1 },
    });
    expect(worst).toBeGreaterThanOrEqual(0);
    expect(worst).toBeLessThan(15);
  });
});

describe('detectIssues', () => {
  it('flags nothing on a good take', () => {
    expect(detectIssues(good)).toEqual([]);
  });

  it('flags the problems found', () => {
    expect(detectIssues({ ...good, sharpness: 0.2, shakiness: 0.8 })).toEqual(['blurry', 'shaky']);
    expect(detectIssues({ ...good, brightness: 0.05 })).toContain('dark');
    expect(detectIssues({ ...good, blackRatio: 1, brightness: 0 })).toEqual(['black']);
    expect(detectIssues({ ...good, audio: { loudnessLufs: null, silenceRatio: 1 } })).toContain('silent');
    expect(detectIssues({ ...good, audio: { loudnessLufs: -40, silenceRatio: 0 } })).toContain('quiet');
  });
});
