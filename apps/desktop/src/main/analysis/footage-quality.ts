import type { LoudnessSample, TimeRange, VideoSample } from './footage-notes-parse.ts';

/**
 * Pure maths for footage notes: planning segments, aggregating raw filter samples into
 * per-segment metrics, and the quality score. No I/O.
 */

export const FOOTAGE_NOTES_VERSION = 1;

/** Scenes longer than this are split into fixed windows so notes stay local in time. */
export const LONG_SCENE_MS = 30_000;
export const WINDOW_MS = 10_000;

export interface SegmentSpec {
  /** index into the asset's scene list */
  sceneIndex: number;
  /** null for a whole short scene, otherwise the window's position within its scene */
  windowIndex: number | null;
  startMs: number;
  endMs: number;
}

/** One segment per scene, except long scenes which become fixed windows (a tiny tail merges into the last). */
export function planSegments(scenes: { startMs: number; endMs: number }[]): SegmentSpec[] {
  const out: SegmentSpec[] = [];
  scenes.forEach((scene, sceneIndex) => {
    const length = scene.endMs - scene.startMs;
    if (length <= LONG_SCENE_MS) {
      out.push({ sceneIndex, windowIndex: null, startMs: scene.startMs, endMs: scene.endMs });
      return;
    }
    let windowIndex = 0;
    for (let start = scene.startMs; start < scene.endMs; start += WINDOW_MS) {
      let end = Math.min(start + WINDOW_MS, scene.endMs);
      if (scene.endMs - end < WINDOW_MS / 3) end = scene.endMs; // absorb a sliver
      out.push({ sceneIndex, windowIndex: windowIndex++, startMs: start, endMs: end });
      if (end === scene.endMs) break;
    }
  });
  return out;
}

export type FootageIssue =
  | 'blurry'
  | 'dark'
  | 'bright'
  | 'low-contrast'
  | 'shaky'
  | 'black'
  | 'frozen'
  | 'silent'
  | 'quiet'
  | 'loud';

export interface SegmentMetrics {
  /** mean luma, 0 (black) to 1 (white) */
  brightness: number;
  /** spread between the 5th and 95th luma percentile, 0..1 */
  contrast: number;
  /** 0 (very soft) to 1 (crisp); from ffmpeg blurdetect at a fixed analysis size */
  sharpness: number;
  /** mean frame-to-frame change (ffmpeg TI), a raw activity measure */
  motion: number;
  /** 0 (steady) to 1 (shaky): sustained frame-wide motion, see {@link shakiness} */
  shakiness: number;
  /** fraction of the segment that is black / frozen, 0..1 */
  blackRatio: number;
  frozenRatio: number;
  /** null when the asset has no audio */
  audio: null | {
    /** energy-mean momentary loudness (LUFS) over non-silent time, null if all silent */
    loudnessLufs: number | null;
    silenceRatio: number;
  };
}

export interface SegmentNotes extends SegmentMetrics {
  sceneIndex: number;
  windowIndex: number | null;
  startMs: number;
  endMs: number;
  /** 0..100 overall take quality, see {@link qualityScore} */
  quality: number;
  issues: FootageIssue[];
  /** on-screen text recognised on the scene keyframe (whole-scene segments and first windows only) */
  text?: string;
}

const clamp01 = (v: number): number => Math.min(1, Math.max(0, v));

function mean(values: number[]): number | null {
  return values.length === 0 ? null : values.reduce((a, b) => a + b, 0) / values.length;
}

function median(values: number[]): number | null {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 ? sorted[mid]! : (sorted[mid - 1]! + sorted[mid]!) / 2;
}

/** Fraction of [startSec, endSec) covered by the union of the given intervals. */
export function overlapRatio(intervals: TimeRange[], startSec: number, endSec: number): number {
  const span = endSec - startSec;
  if (span <= 0) return 0;
  const clipped = intervals
    .map((i) => ({ s: Math.max(i.startSec, startSec), e: Math.min(i.endSec, endSec) }))
    .filter((i) => i.e > i.s)
    .sort((a, b) => a.s - b.s);
  let covered = 0;
  let cursor = startSec;
  for (const { s, e } of clipped) {
    const from = Math.max(s, cursor);
    if (e > from) {
      covered += e - from;
      cursor = e;
    }
  }
  return clamp01(covered / span);
}

/**
 * blurdetect reports an average edge width in pixels at the analysis resolution (320 px wide):
 * about 4 for crisp footage, 5.7 for a 8x downscale-blur, 8.6+ for heavy blur (measured on the
 * bundled build). Mapped linearly from 3 (1.0) to 9 (0.0).
 */
export function sharpnessFromBlur(blur: number): number {
  return clamp01(1 - (blur - 3) / 6);
}

/**
 * Camera shake is approximated from temporal information. TI is the stdev of the frame
 * difference, so a locked-off shot sits near 0 while whole-frame shake keeps it high at all
 * times. The median (not the mean) is used so that a cut or one subject movement in an
 * otherwise steady shot does not read as shake. Heuristic: ffmpeg's vidstab is not in the
 * bundled build. 3 or less is steady, 15 or more is fully shaky (at 4 samples/s).
 */
export function shakiness(tiValues: number[]): number {
  const m = median(tiValues);
  if (m === null) return 0;
  return clamp01((m - 3) / 12);
}

/** Aggregates the raw filter output over one [startMs, endMs) segment. */
export function aggregateSegment(input: {
  startMs: number;
  endMs: number;
  video: VideoSample[];
  black: TimeRange[];
  frozen: TimeRange[];
  /** undefined when the asset has no audio track */
  loudness?: LoudnessSample[];
  silence?: TimeRange[];
}): SegmentMetrics {
  const s = input.startMs / 1000;
  const e = input.endMs / 1000;
  const inSeg = input.video.filter((v) => v.t >= s && v.t < e);
  const pick = (f: (v: VideoSample) => number | undefined): number[] =>
    inSeg.map(f).filter((x): x is number => x !== undefined);

  const yAvg = mean(pick((v) => v.yAvg));
  const lows = pick((v) => v.yLow);
  const highs = pick((v) => v.yHigh);
  const blur = median(pick((v) => v.blur));
  const ti = pick((v) => v.ti);

  const contrastRaw = mean(highs) !== null && mean(lows) !== null ? (mean(highs)! - mean(lows)!) / 219 : 0;
  // limited-range luma: 16 is black and 235 is white
  const brightness = yAvg === null ? 0 : clamp01((yAvg - 16) / 219);

  let audio: SegmentMetrics['audio'] = null;
  if (input.loudness) {
    const samples = input.loudness.filter((l) => l.t >= s && l.t < e);
    const audible = samples.filter((l) => l.m > -70);
    const energy = mean(audible.map((l) => Math.pow(10, l.m / 10)));
    audio = {
      loudnessLufs: energy === null ? null : round(10 * Math.log10(energy), 1),
      silenceRatio: overlapRatio(input.silence ?? [], s, e),
    };
  }

  return {
    brightness: round(brightness, 3),
    contrast: round(clamp01(contrastRaw), 3),
    sharpness: round(blur === null ? 0.5 : sharpnessFromBlur(blur), 3),
    motion: round(mean(ti) ?? 0, 2),
    shakiness: round(shakiness(ti), 3),
    blackRatio: round(overlapRatio(input.black, s, e), 3),
    frozenRatio: round(overlapRatio(input.frozen, s, e), 3),
    audio,
  };
}

function round(v: number, digits: number): number {
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

/** Closeness to a target: 1 at the target, falling linearly to 0 at `reach` away. */
function nearness(value: number, target: number, reach: number): number {
  return clamp01(1 - Math.abs(value - target) / reach);
}

/** Loudness fit for dialogue/b-roll: -23 to -14 LUFS is fine, quieter or louder falls off. */
function audioScore(a: NonNullable<SegmentMetrics['audio']>): number {
  if (a.loudnessLufs === null) return 0;
  let level: number;
  if (a.loudnessLufs < -23) level = clamp01(1 - (-23 - a.loudnessLufs) / 20); // -43 LUFS scores 0
  else if (a.loudnessLufs > -14) level = clamp01(1 - (a.loudnessLufs + 14) / 8); // -6 LUFS scores 0
  else level = 1;
  return level * (1 - a.silenceRatio);
}

/**
 * Take quality, 0..100, "sharp, well-lit, steady, audible":
 *   sharpness 35% + exposure 25% + steadiness 25% + audio 15%
 * where exposure blends how close brightness is to mid-grey (0.45) with contrast. Without an
 * audio track the audio weight is redistributed. The result is then scaled down by the
 * fraction of black frames and (less) frozen frames, since those are unusable footage.
 */
export function qualityScore(m: SegmentMetrics): number {
  const exposure = 0.7 * nearness(m.brightness, 0.45, 0.45) + 0.3 * clamp01(m.contrast / 0.5);
  const steadiness = 1 - m.shakiness;
  const parts: [number, number][] = [
    [0.35, m.sharpness],
    [0.25, exposure],
    [0.25, steadiness],
  ];
  if (m.audio) parts.push([0.15, audioScore(m.audio)]);
  const totalWeight = parts.reduce((a, [w]) => a + w, 0);
  const base = parts.reduce((a, [w, v]) => a + w * v, 0) / totalWeight;
  const usable = (1 - m.blackRatio) * (1 - 0.8 * m.frozenRatio);
  return Math.round(clamp01(base * usable) * 100);
}

/** Human-readable problems worth surfacing, for search and for the agent later. */
export function detectIssues(m: SegmentMetrics): FootageIssue[] {
  const issues: FootageIssue[] = [];
  if (m.blackRatio >= 0.5) issues.push('black');
  if (m.frozenRatio >= 0.5) issues.push('frozen');
  if (m.blackRatio < 0.5) {
    if (m.sharpness < 0.4) issues.push('blurry');
    if (m.brightness < 0.15) issues.push('dark');
    else if (m.brightness > 0.85) issues.push('bright');
    if (m.contrast < 0.15 && m.brightness >= 0.15) issues.push('low-contrast');
  }
  if (m.shakiness >= 0.5) issues.push('shaky');
  if (m.audio) {
    if (m.audio.silenceRatio >= 0.9 || m.audio.loudnessLufs === null) issues.push('silent');
    else if (m.audio.loudnessLufs < -35) issues.push('quiet');
    else if (m.audio.loudnessLufs > -9) issues.push('loud');
  }
  return issues;
}

export function scoreSegment(spec: SegmentSpec, metrics: SegmentMetrics): SegmentNotes {
  return {
    ...spec,
    ...metrics,
    quality: qualityScore(metrics),
    issues: detectIssues(metrics),
  };
}
