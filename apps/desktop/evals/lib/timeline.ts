import type { Item, TimelineDoc } from '@cutboard/schema';

/**
 * Pure helpers that read a finished timeline doc. Checks score the RESULT (what is on the
 * timeline), never the agent's wording, so everything here works on frames/seconds of the doc.
 */

export interface Interval {
  start: number;
  end: number;
}

export const MEDIA_TYPES: Item['type'][] = ['video', 'audio', 'image'];

export const endFrame = (item: Item): number => item.startFrame + item.durationFrames;

/** Last frame of any item, like the editor's own duration. */
export function docDurationSec(doc: TimelineDoc): number {
  return doc.items.reduce((max, i) => Math.max(max, endFrame(i)), 0) / doc.project.fps;
}

/** Items of one type, left to right. */
export function itemsOfType(doc: TimelineDoc, type: Item['type']): Item[] {
  return doc.items.filter((i) => i.type === type).sort((a, b) => a.startFrame - b.startFrame);
}

export function itemsOfAsset(doc: TimelineDoc, assetId: string | undefined): Item[] {
  if (!assetId) return [];
  return doc.items.filter((i) => i.assetId === assetId).sort((a, b) => a.startFrame - b.startFrame);
}

export function mergeIntervals(intervals: Interval[]): Interval[] {
  const sorted = intervals.filter((i) => i.end > i.start).sort((a, b) => a.start - b.start);
  const out: Interval[] = [];
  for (const iv of sorted) {
    const last = out[out.length - 1];
    if (last && iv.start <= last.end) last.end = Math.max(last.end, iv.end);
    else out.push({ ...iv });
  }
  return out;
}

/** Seconds of `span` covered by `intervals`. */
export function coveredSec(span: Interval, intervals: Interval[]): number {
  let total = 0;
  for (const iv of mergeIntervals(intervals)) {
    total += Math.max(0, Math.min(span.end, iv.end) - Math.max(span.start, iv.start));
  }
  return total;
}

/** Source-media seconds an item plays: [sourceIn, sourceIn + duration * speed]. */
export function sourceInterval(item: Item, fps: number): Interval {
  const start = (item.sourceInFrame ?? 0) / fps;
  return { start, end: start + (item.durationFrames * item.speed) / fps };
}

/** Parts of an asset's source still on the timeline (any video/audio item that plays it). */
export function keptSource(doc: TimelineDoc, assetId: string | undefined): Interval[] {
  const fps = doc.project.fps;
  return mergeIntervals(
    itemsOfAsset(doc, assetId)
      .filter((i) => i.type === 'video' || i.type === 'audio')
      .map((i) => sourceInterval(i, fps)),
  );
}

/** Fraction (0..1) of a source span that is still played somewhere on the timeline. */
export function keptFraction(doc: TimelineDoc, assetId: string | undefined, span: Interval): number {
  const len = span.end - span.start;
  if (len <= 0) return 1;
  return Math.min(1, coveredSec(span, keptSource(doc, assetId)) / len);
}

/** Timeline seconds covered by items of the given types. */
export function timelineCoverage(doc: TimelineDoc, types: Item['type'][]): Interval[] {
  const fps = doc.project.fps;
  return mergeIntervals(doc.items.filter((i) => types.includes(i.type)).map((i) => ({ start: i.startFrame / fps, end: endFrame(i) / fps })));
}

/**
 * Volume an item plays at a timeline frame. Volume keyframes (item-local frames) replace the
 * static volume, linearly interpolated, like the exporter does.
 */
export function volumeAt(item: Item, timelineFrame: number): number {
  const kfs = [...(item.keyframes?.['volume'] ?? [])].sort((a, b) => a.frame - b.frame);
  if (kfs.length === 0) return item.muted ? 0 : item.volume;
  const f = timelineFrame - item.startFrame;
  const first = kfs[0]!;
  const last = kfs[kfs.length - 1]!;
  if (f <= first.frame) return first.value;
  if (f >= last.frame) return last.value;
  for (let i = 0; i < kfs.length - 1; i++) {
    const a = kfs[i]!;
    const b = kfs[i + 1]!;
    if (f >= a.frame && f <= b.frame) {
      if (a.easing === 'hold' || b.frame === a.frame) return a.value;
      return a.value + ((b.value - a.value) * (f - a.frame)) / (b.frame - a.frame);
    }
  }
  return last.value;
}

/** Mean volume of an item over [startSec, endSec) of the timeline, sampled every 50 ms. */
export function meanVolume(item: Item, span: Interval, fps: number): number {
  const step = 0.05;
  let sum = 0;
  let n = 0;
  for (let t = span.start + step / 2; t < span.end; t += step) {
    sum += volumeAt(item, Math.round(t * fps));
    n++;
  }
  return n === 0 ? volumeAt(item, Math.round(span.start * fps)) : sum / n;
}

/** Lowercase words with punctuation stripped. */
export function tokens(text: string): string[] {
  return text
    .toLowerCase()
    .replace(/[^a-z0-9'\s]/g, ' ')
    .split(/\s+/)
    .filter(Boolean);
}

/** Sort items on one track and report overlaps and gaps (seconds) between neighbours. */
export function trackLayout(items: Item[], fps: number): { overlapSec: number; maxGapSec: number } {
  const sorted = [...items].sort((a, b) => a.startFrame - b.startFrame);
  let overlap = 0;
  let maxGap = 0;
  for (let i = 1; i < sorted.length; i++) {
    const delta = (sorted[i]!.startFrame - endFrame(sorted[i - 1]!)) / fps;
    if (delta < 0) overlap += -delta;
    else maxGap = Math.max(maxGap, delta);
  }
  return { overlapSec: overlap, maxGapSec: maxGap };
}
