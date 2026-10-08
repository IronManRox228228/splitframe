import { describe, expect, it } from 'vitest';
import { analyzePcm, BEAT_SAMPLE_RATE } from './beat-dsp.ts';

/** Click track: a decaying 1.5 kHz tick plus a 60 Hz thump on every beat, an accent on every 4th. */
function clickTrack(bpm: number, firstBeatSec: number, seconds: number): Int16Array {
  const n = seconds * BEAT_SAMPLE_RATE;
  const pcm = new Int16Array(n);
  const period = 60 / bpm;
  for (let beat = 0; firstBeatSec + beat * period < seconds; beat++) {
    const start = Math.round((firstBeatSec + beat * period) * BEAT_SAMPLE_RATE);
    const gain = beat % 4 === 0 ? 0.9 : 0.6;
    for (let i = 0; i < 4000 && start + i < n; i++) {
      const t = i / BEAT_SAMPLE_RATE;
      const v = gain * (0.5 * Math.sin(2 * Math.PI * 1500 * t) * Math.exp(-90 * t) + 0.8 * Math.sin(2 * Math.PI * 60 * t) * Math.exp(-14 * t));
      pcm[start + i] = Math.round(v * 20000);
    }
  }
  return pcm;
}

/** Largest distance from a true beat to the nearest detected one, over the first `count` beats. */
function worstError(detectedMs: number[], bpm: number, firstBeatSec: number, count: number): number {
  let worst = 0;
  for (let k = 0; k < count; k++) {
    const truth = (firstBeatSec + (k * 60) / bpm) * 1000;
    worst = Math.max(worst, Math.min(...detectedMs.map((d) => Math.abs(d - truth))));
  }
  return worst;
}

describe('beat detection on a synthetic click track', () => {
  it('finds exactly 120 BPM with the first beat at 0 (was 121 BPM, grid ~0.2 s late)', () => {
    const map = analyzePcm(clickTrack(120, 0, 60));
    expect(map.bpm).toBeCloseTo(120, 0);
    expect(worstError(map.beatsMs, 120, 0, 110)).toBeLessThanOrEqual(20);
  });

  it('keeps a phase offset and a non-integer-lag tempo', () => {
    const map = analyzePcm(clickTrack(100, 0.13, 40));
    expect(Math.abs(map.bpm - 100)).toBeLessThanOrEqual(0.5);
    expect(worstError(map.beatsMs, 100, 0.13, 60)).toBeLessThanOrEqual(20);
  });

  it('does not report half the tempo when the bar accent is stronger', () => {
    const map = analyzePcm(clickTrack(140, 0.05, 40));
    expect(Math.abs(map.bpm - 140)).toBeLessThanOrEqual(0.7);
  });

  it('marks downbeats on the accented beats', () => {
    const map = analyzePcm(clickTrack(120, 0, 30));
    expect(Math.abs(map.downbeatsMs[1]! - 2000)).toBeLessThanOrEqual(20);
    expect(map.downbeatsMs.length).toBeLessThan(map.beatsMs.length / 3);
  });
});
