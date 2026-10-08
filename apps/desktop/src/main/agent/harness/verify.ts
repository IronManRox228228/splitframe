import type { Item, TimelineDoc } from '@cutboard/schema';
import { keptSourceSec } from './compiler.ts';
import type { Finding } from './types.ts';
import { fmtDur, fmtTime } from './units.ts';

/**
 * The verifier: deterministic checks of the timeline after a step, comparing what the step
 * claimed with what the document now says. Failures drive retries; the final reply is built
 * from what these checks (and the tool results) confirm, never from the model's own claims.
 */

const fail = (message: string): Finding => ({ level: 'fail', message });
const warn = (message: string): Finding => ({ level: 'warn', message });

export const docLengthFrames = (doc: TimelineDoc): number => doc.items.reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);

export const hasFailure = (findings: Finding[]): boolean => findings.some((f) => f.level === 'fail');

/** The step claimed to change the project and the document is identical. */
export function changedSomething(before: TimelineDoc, after: TimelineDoc, what: string): Finding[] {
  return JSON.stringify({ i: before.items, p: before.project, t: before.tracks }) === JSON.stringify({ i: after.items, p: after.project, t: after.tracks })
    ? [fail(`${what} changed nothing on the timeline`)]
    : [];
}

/** Overlaps between items of one track that were not there before the step. */
export function newOverlaps(before: TimelineDoc, after: TimelineDoc, types: Item['type'][] = ['video']): Finding[] {
  const count = (doc: TimelineDoc) => {
    let overlap = 0;
    for (const track of doc.tracks) {
      const items = doc.items.filter((i) => i.trackId === track.id && types.includes(i.type)).sort((a, b) => a.startFrame - b.startFrame);
      for (let k = 1; k < items.length; k++) {
        const prevEnd = items[k - 1]!.startFrame + items[k - 1]!.durationFrames;
        overlap += Math.max(0, prevEnd - items[k]!.startFrame);
      }
    }
    return overlap;
  };
  const fps = after.project.fps;
  const a = count(after);
  return a > count(before) + 2 ? [fail(`clips now overlap by ${fmtDur(a / fps)} on a track`)] : [];
}

export function lengthNear(doc: TimelineDoc, targetSec: number, tolSec: number): Finding[] {
  const len = docLengthFrames(doc) / doc.project.fps;
  return Math.abs(len - targetSec) > tolSec ? [fail(`the timeline is ${fmtDur(len)} long, wanted about ${fmtDur(targetSec)}`)] : [];
}

export function itemRange(doc: TimelineDoc, itemId: string, startFrame: number, durationFrames: number): Finding[] {
  const item = doc.items.find((i) => i.id === itemId);
  if (!item) return [fail('the item is no longer on the timeline')];
  const out: Finding[] = [];
  if (Math.abs(item.startFrame - startFrame) > 1) out.push(fail(`it starts at ${fmtTime(item.startFrame / doc.project.fps)}, expected ${fmtTime(startFrame / doc.project.fps)}`));
  if (Math.abs(item.durationFrames - durationFrames) > 1) out.push(fail(`it lasts ${fmtDur(item.durationFrames / doc.project.fps)}, expected ${fmtDur(durationFrames / doc.project.fps)}`));
  return out;
}

export function itemsGone(doc: TimelineDoc, ids: string[]): Finding[] {
  const still = ids.filter((id) => doc.items.some((i) => i.id === id));
  return still.length > 0 ? [fail(`${still.length} clip(s) are still on the timeline`)] : [];
}

/** Items in the given order, each starting at or after the previous one's end (no overlaps, gaps tolerated up to maxGapSec). */
export function inOrder(doc: TimelineDoc, ids: string[], maxGapSec = 0.1): Finding[] {
  const items = ids.map((id) => doc.items.find((i) => i.id === id));
  if (items.some((i) => !i)) return [fail('a clip went missing while reordering')];
  const fps = doc.project.fps;
  const out: Finding[] = [];
  for (let k = 1; k < items.length; k++) {
    const prev = items[k - 1]!;
    const cur = items[k]!;
    const gap = (cur.startFrame - (prev.startFrame + prev.durationFrames)) / fps;
    if (gap < -0.05) out.push(fail('clips overlap after the move'));
    else if (gap > maxGapSec) out.push(fail(`a ${fmtDur(gap)} gap was left between clips`));
  }
  return out;
}

export function titleExists(doc: TimelineDoc, text: string, startFrame: number, durationFrames: number): Finding[] {
  const hit = doc.items.find((i) => i.type === 'text' && String((i.props as { text?: string }).text ?? '').trim() === text.trim());
  if (!hit) return [fail(`no title reading "${text}" is on the timeline`)];
  const out = itemRange(doc, hit.id, startFrame, durationFrames);
  const track = doc.tracks.find((t) => t.id === hit.trackId);
  if (track?.kind !== 'text') out.push(fail('the title is not on a text track'));
  return out;
}

/** Caption cards cover the speaking part of the media, sit on a text track and do not run past the media. */
export function captionCoverage(doc: TimelineDoc, minCover = 0.6): Finding[] {
  const cards = doc.items.filter((i) => i.type === 'caption');
  if (cards.length === 0) return [fail('no caption cards were created')];
  const fps = doc.project.fps;
  const media = doc.items.filter((i) => i.type === 'video' || i.type === 'audio');
  const mediaEnd = Math.max(0, ...media.map((i) => i.startFrame + i.durationFrames));
  const out: Finding[] = [];
  const merged: [number, number][] = [];
  for (const c of [...cards].sort((a, b) => a.startFrame - b.startFrame)) {
    const last = merged[merged.length - 1];
    if (last && c.startFrame <= last[1]) last[1] = Math.max(last[1], c.startFrame + c.durationFrames);
    else merged.push([c.startFrame, c.startFrame + c.durationFrames]);
  }
  const covered = merged.reduce((a, [s, e]) => a + (e - s), 0);
  const speechFrames = mediaEnd; // media length is the best proxy once pauses are cut
  if (speechFrames > 0 && covered / speechFrames < minCover) out.push(fail(`captions cover only ${Math.round((covered / speechFrames) * 100)}% of the video`));
  const textTrackIds = new Set(doc.tracks.filter((t) => t.kind === 'text').map((t) => t.id));
  if (cards.some((c) => !textTrackIds.has(c.trackId))) out.push(fail('some caption cards are not on a text track'));
  const overrun = Math.max(...cards.map((c) => c.startFrame + c.durationFrames)) - mediaEnd;
  if (overrun / fps > 0.5) out.push(fail(`captions run ${fmtDur(overrun / fps)} past the end of the video`));
  return out;
}

export function captionFontSize(doc: TimelineDoc): number | undefined {
  const c = doc.items.find((i) => i.type === 'caption');
  return c && c.type === 'caption' ? c.props.style.fontSize : undefined;
}

/** After a pause cut: no silence longer than `limitSec` (plus the handles the cut keeps) is still fully on the timeline. */
export function pausesGone(doc: TimelineDoc, silencesByAsset: Record<string, { startMs: number; endMs: number }[] | null>, thresholdSec: number, slackSec = 0.25): Finding[] {
  const fps = doc.project.fps;
  let left = 0;
  let worst = 0;
  for (const [assetId, silences] of Object.entries(silencesByAsset)) {
    for (const s of silences ?? []) {
      const a = s.startMs / 1000;
      const b = s.endMs / 1000;
      if (b - a <= thresholdSec) continue;
      const kept = keptSourceSec(doc.items, assetId, fps, a, b);
      if (kept > thresholdSec + slackSec) {
        left++;
        worst = Math.max(worst, kept);
      }
    }
  }
  return left > 0 ? [fail(`${left} pause(s) longer than ${fmtDur(thresholdSec)} are still there (longest ${fmtDur(worst)})`)] : [];
}

/** After a filler cut: each filler range is mostly gone. */
export function fillersGone(doc: TimelineDoc, assetId: string, ranges: [number, number][]): Finding[] {
  const fps = doc.project.fps;
  const left = ranges.filter(([a, b]) => keptSourceSec(doc.items, assetId, fps, a, b) / Math.max(0.01, b - a) > 0.25);
  return left.length > 0 ? [fail(`${left.length} of ${ranges.length} filler words are still in the video`)] : [];
}

export function musicPlaced(doc: TimelineDoc, itemId: string, coverFrames: number, maxVolume: number): Finding[] {
  const m = doc.items.find((i) => i.id === itemId);
  if (!m) return [fail('the music item is not on the timeline')];
  const out: Finding[] = [];
  if (m.startFrame > doc.project.fps / 4) out.push(fail(`the music starts at ${fmtTime(m.startFrame / doc.project.fps)} instead of the beginning`));
  if (m.durationFrames < coverFrames * 0.95) out.push(warn(`the music covers ${fmtDur(m.durationFrames / doc.project.fps)} of ${fmtDur(coverFrames / doc.project.fps)} (the track is shorter)`));
  if (m.volume > maxVolume + 0.001) out.push(fail(`the music volume is ${m.volume}, expected at most ${maxVolume}`));
  return out;
}

export function canvasIs(doc: TimelineDoc, aspect: string): Finding[] {
  const [aw, ah] = aspect.split(':').map(Number) as [number, number];
  const ratio = doc.project.width / doc.project.height;
  return Math.abs(ratio - aw / ah) > 0.02 ? [fail(`the canvas is ${doc.project.width}x${doc.project.height}, not ${aspect}`)] : [];
}

/** Sum of a clip's volume keyframes shows the envelope was written. */
export function hasVolumeEnvelope(doc: TimelineDoc, itemId: string): boolean {
  return (doc.items.find((i) => i.id === itemId)?.keyframes?.['volume']?.length ?? 0) > 0;
}
