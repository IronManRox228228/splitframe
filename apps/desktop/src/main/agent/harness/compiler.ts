import { createItem, itemEnd } from '@cutboard/editor-core';
import { newId, type Item, type Op, type TranscriptWord } from '@cutboard/schema';
import { cutSpansOps, findRetakeSpans, mergeSpans } from '@cutboard/tools';
import type { Snapshot } from './types.ts';

/**
 * The compiler: deterministic implementations of the mechanical steps. Everything here is pure
 * (snapshot in, ops out) and takes seconds; the frame math, ids, ripple and placement live here
 * so the model never has to do any of it.
 */

export type Span = { startSec: number; endSec: number };
type FrameSpan = { startFrame: number; endFrame: number };

const isMedia = (i: Item): boolean => i.type === 'video' || i.type === 'audio';

/** The source window an item plays: [in, out) in seconds of its asset. */
export function sourceWindow(item: Item, fps: number): Span {
  const startSec = (item.sourceInFrame ?? 0) / fps;
  return { startSec, endSec: startSec + (item.durationFrames * item.speed) / fps };
}

/** Timeline frames of the part of a source range [a, b] seconds this item plays; null when it plays none of it. */
export function sourceSpanToTimeline(item: Item, fps: number, a: number, b: number): FrameSpan | null {
  const win = sourceWindow(item, fps);
  const from = Math.max(a, win.startSec);
  const to = Math.min(b, win.endSec);
  if (to - from <= 1 / fps) return null;
  const startFrame = item.startFrame + Math.round(((from - win.startSec) * fps) / item.speed);
  const endFrame = item.startFrame + Math.round(((to - win.startSec) * fps) / item.speed);
  return endFrame > startFrame ? { startFrame, endFrame } : null;
}

/** Seconds of the source range [a, b] still played by items of this asset (counted once per item). */
export function keptSourceSec(items: Item[], assetId: string, fps: number, a: number, b: number): number {
  let kept = 0;
  for (const item of items) {
    if (item.assetId !== assetId || !isMedia(item)) continue;
    const win = sourceWindow(item, fps);
    kept += Math.max(0, Math.min(b, win.endSec) - Math.max(a, win.startSec));
  }
  return kept;
}

/** Cut source ranges (seconds of the asset) out of every item playing that asset, ripple-closing the gaps. */
export function cutSourceRangesOps(snap: Pick<Snapshot, 'doc'>, assetId: string, ranges: [number, number][], onlyItemIds?: string[]): { ops: Op[]; cutSec: number } {
  const fps = snap.doc.project.fps;
  const items = snap.doc.items
    .filter((i) => i.assetId === assetId && isMedia(i) && (!onlyItemIds || onlyItemIds.includes(i.id)))
    .sort((a, b) => b.startFrame - a.startFrame); // later items first: earlier coordinates stay valid
  const ops: Op[] = [];
  let cutFrames = 0;
  for (const item of items) {
    const spans = mergeSpans(ranges.map(([a, b]) => sourceSpanToTimeline(item, fps, a, b)).filter((s): s is FrameSpan => s !== null));
    for (const s of spans) cutFrames += s.endFrame - s.startFrame;
    ops.push(...cutSpansOps(item, spans));
  }
  return { ops, cutSec: cutFrames / fps };
}

// ---------- fillers ----------

export const DEFAULT_FILLERS = ['um', 'uh', 'er', 'ah', 'hmm', 'you know'];

const clean = (w: string): string => w.toLowerCase().replace(/[^\p{L}\p{N}']/gu, '');

/** Stretched spellings of the sound fillers ("umm", "uhhh", "errr", "hmmm"). */
const SOUND: Record<string, RegExp> = { um: /^u+m+$/, uh: /^u+h+$/, er: /^e+r+m*$/, erm: /^e+r+m+$/, ah: /^a+h+$/, hmm: /^h+m+$/, mm: /^m{2,}$/ };

function matchesToken(token: string, vocabWord: string): boolean {
  const re = SOUND[vocabWord];
  return re ? re.test(token) : token === vocabWord;
}

/** Filler words and phrases in a transcript, as source ranges in seconds (ASR end times stretch over pauses, so they are capped). */
export function fillerRanges(words: TranscriptWord[], vocab: string[] = DEFAULT_FILLERS, maxSec = 0.8): [number, number][] {
  const phrases = vocab.map((v) => v.toLowerCase().split(/\s+/).map(clean).filter(Boolean)).filter((p) => p.length > 0);
  const tokens = words.map((w) => clean(w.w));
  const out: [number, number][] = [];
  for (let i = 0; i < words.length; i++) {
    for (const phrase of phrases) {
      if (i + phrase.length > words.length) continue;
      if (!phrase.every((p, k) => matchesToken(tokens[i + k]!, p))) continue;
      const last = words[i + phrase.length - 1]!;
      const next = words[i + phrase.length];
      // ASR starts a filler late (the leading breath or vowel is attributed to the pause before it): reach back a little (ASR ends of the previous word stretch over the filler, so they are no guide)
      const prev = words[i - 1];
      const startMs = Math.max(prev ? prev.startMs + 200 : 0, words[i]!.startMs - 350);
      const endMs = Math.min(last.endMs, next ? Math.max(next.startMs, startMs + 100) : last.endMs, words[i]!.startMs + maxSec * 1000);
      out.push([startMs / 1000, endMs / 1000]);
      i += phrase.length - 1;
      break;
    }
  }
  return out;
}

// ---------- retakes ----------

/** Cut spans (timeline frames) for repeated takes on one item: the earlier attempt goes, the last stays. */
export function retakeOps(snap: Snapshot, item: Item, minWords = 5): { ops: Op[]; takes: number } {
  const spans = findRetakeSpans(snap as never, item, minWords).map((s) => ({ startFrame: s.startFrame, endFrame: s.endFrame }));
  return { ops: cutSpansOps(item, mergeSpans(spans)), takes: spans.length };
}

// ---------- sentences and picks (judgment steps) ----------

export interface Sentence {
  index: number;
  startSec: number;
  endSec: number;
  text: string;
}

/** Transcript split into numbered sentences at sentence punctuation (or after 30 words). */
export function sentencesOf(words: TranscriptWord[]): Sentence[] {
  const out: Sentence[] = [];
  let buf: TranscriptWord[] = [];
  const flush = () => {
    if (buf.length === 0) return;
    out.push({ index: out.length + 1, startSec: buf[0]!.startMs / 1000, endSec: buf[buf.length - 1]!.endMs / 1000, text: buf.map((w) => w.w).join(' ') });
    buf = [];
  };
  for (const w of words) {
    buf.push(w);
    if (/[.!?]["')]?$/.test(w.w.trim()) || buf.length >= 30) flush();
  }
  flush();
  return out;
}

/** ASR end times stretch over the pause that follows a sentence: pull ends in to where the audio goes quiet. */
export function snapToSpeech(sentences: Sentence[], silences: { startMs: number; endMs: number }[] | null, handleSec = 0.12): Sentence[] {
  if (!silences || silences.length === 0) return sentences;
  return sentences.map((s) => {
    let { startSec, endSec } = s;
    for (const sil of silences) {
      const a = sil.startMs / 1000;
      const b = sil.endMs / 1000;
      if (a > startSec + 0.2 && a < endSec && b >= endSec - 0.05) endSec = Math.max(a + handleSec, startSec + 0.2);
      if (b > startSec && a <= startSec + 0.05 && b < endSec - 0.2) startSec = Math.max(startSec, b - handleSec);
    }
    return { ...s, startSec, endSec };
  });
}

export interface SentencePick {
  first: number;
  last: number;
}

/**
 * Turn the model's picks (sentence numbers, in play order) into source ranges that fit the target length:
 * whole sentences only, trimmed from the end while that brings the total closer to the target.
 * Sentences less than `joinGapSec` apart become one range; longer gaps stay cut out.
 */
export function fitPicks(sentences: Sentence[], picks: SentencePick[], targetSec: number | undefined, joinGapSec = 0.4): { ranges: Span[]; totalSec: number; sentenceCount: number } {
  const chosen: Sentence[] = [];
  const seen = new Set<number>();
  for (const p of picks) {
    const lo = Math.max(1, Math.min(p.first, p.last));
    const hi = Math.min(sentences.length, Math.max(p.first, p.last));
    for (let n = lo; n <= hi; n++) {
      if (seen.has(n)) continue;
      seen.add(n);
      chosen.push(sentences[n - 1]!);
    }
  }
  const len = (s: Sentence) => s.endSec - s.startSec;
  let total = chosen.reduce((a, s) => a + len(s), 0);
  if (targetSec !== undefined) {
    while (chosen.length > 1) {
      const last = chosen[chosen.length - 1]!;
      if (Math.abs(total - len(last) - targetSec) < Math.abs(total - targetSec)) {
        chosen.pop();
        total -= len(last);
      } else break;
    }
  }
  const ranges: Span[] = [];
  for (const s of chosen) {
    const prev = ranges[ranges.length - 1];
    if (prev && s.startSec >= prev.endSec - 0.01 && s.startSec - prev.endSec <= joinGapSec) prev.endSec = s.endSec;
    else ranges.push({ startSec: s.startSec, endSec: s.endSec });
  }
  return { ranges, totalSec: total, sentenceCount: chosen.length };
}

// ---------- assemble (keep source ranges in this order) ----------

export interface SourceRange {
  assetId: string;
  startSec: number;
  endSec: number;
}

/**
 * Lay source ranges back to back on the main video track, in the order given. Existing video items of the
 * same assets on that track are replaced; the sequence starts at the earliest of them (or 0).
 */
export function assembleOps(snap: Pick<Snapshot, 'doc' | 'assets'>, ranges: SourceRange[]): { ops: Op[]; totalFrames: number; itemIds: string[] } {
  const fps = snap.doc.project.fps;
  const first = snap.assets.find((a) => a.id === ranges[0]?.assetId);
  const wantAudio = first?.kind === 'audio';
  const track = snap.doc.tracks.find((t) => !t.locked && t.kind === (wantAudio ? 'audio' : 'video'));
  if (!track) throw new Error(`No unlocked ${wantAudio ? 'audio' : 'video'} track to build the sequence on.`);
  const assetIds = new Set(ranges.map((r) => r.assetId));
  const doomed = snap.doc.items.filter((i) => i.trackId === track.id && i.assetId && assetIds.has(i.assetId));
  const ops: Op[] = [];
  if (doomed.length > 0) ops.push({ type: 'item.remove', itemIds: doomed.map((i) => i.id), ripple: false });
  let cursor = doomed.length > 0 ? Math.min(...doomed.map((i) => i.startFrame)) : 0;
  const itemIds: string[] = [];
  const start = cursor;
  for (const r of ranges) {
    const asset = snap.assets.find((a) => a.id === r.assetId)!;
    const sourceIn = Math.max(0, Math.round(r.startSec * fps));
    const maxEnd = Math.round((asset.durationMs / 1000) * fps);
    const dur = Math.max(1, Math.min(Math.round(r.endSec * fps), maxEnd) - sourceIn);
    const id = newId('itm');
    ops.push({
      type: 'item.add',
      item: createItem(asset.kind === 'audio' ? 'audio' : 'video', {
        id,
        trackId: track.id,
        startFrame: cursor,
        durationFrames: dur,
        assetId: asset.id,
        sourceInFrame: sourceIn,
        labels: { name: asset.originalName },
      } as never) as never,
    });
    itemIds.push(id);
    cursor += dur;
  }
  return { ops, totalFrames: cursor - start, itemIds };
}

// ---------- trim / reorder ----------

/** Keep only source seconds [from, to] of a clip. Later items on the track follow when `ripple` (their gaps stay). */
export function setClipRangeOps(snap: Pick<Snapshot, 'doc' | 'assets'>, item: Item, fromSec: number, toSec: number, ripple: boolean): { ops: Op[]; newDurationFrames: number } {
  const fps = snap.doc.project.fps;
  const asset = snap.assets.find((a) => a.id === item.assetId);
  const maxSec = asset && asset.durationMs > 0 ? asset.durationMs / 1000 : Infinity;
  const a = Math.max(0, Math.min(fromSec, maxSec));
  const b = Math.min(toSec, maxSec);
  if (b - a < 1 / fps) throw new Error(`The range ${a.toFixed(1)}-${b.toFixed(1)}s is empty (the source is ${Number.isFinite(maxSec) ? maxSec.toFixed(1) : '?'}s long).`);
  const newDur = Math.max(1, Math.round(((b - a) * fps) / item.speed));
  const ops: Op[] = [{ type: 'item.update', itemId: item.id, patch: { sourceInFrame: Math.round(a * fps), durationFrames: newDur } as never }];
  const delta = newDur - item.durationFrames;
  if (ripple && delta !== 0) {
    for (const other of snap.doc.items) {
      if (other.id === item.id || other.trackId !== item.trackId || other.startFrame < itemEnd(item)) continue;
      ops.push({ type: 'item.move', itemId: other.id, startFrame: Math.max(0, other.startFrame + delta) });
    }
  }
  return { ops, newDurationFrames: newDur };
}

/** Put the given items of one track back to back in this order, starting where the earliest of them started. */
export function layoutInOrderOps(items: Item[]): Op[] {
  let cursor = Math.min(...items.map((i) => i.startFrame));
  const ops: Op[] = [];
  for (const item of items) {
    if (item.startFrame !== cursor) ops.push({ type: 'item.move', itemId: item.id, startFrame: cursor });
    cursor += item.durationFrames;
  }
  return ops;
}

/** The order after moving `moving` before/after `anchor` on its track (time order of the rest is kept). */
export function reorderedItems(trackItems: Item[], moving: Item, anchor: Item, where: 'before' | 'after'): Item[] {
  const rest = [...trackItems].sort((a, b) => a.startFrame - b.startFrame || a.id.localeCompare(b.id)).filter((i) => i.id !== moving.id);
  const at = rest.findIndex((i) => i.id === anchor.id);
  rest.splice(where === 'before' ? at : at + 1, 0, moving);
  return rest;
}

// ---------- captions ----------

/** Rescale caption text and/or recolor / reposition every caption card, as one batch. */
export function captionStyleOps(items: Item[], change: { sizeFactor?: number; fontSize?: number; color?: string; placementY?: number }): Op[] {
  const ops: Op[] = [];
  for (const item of items) {
    if (item.type !== 'caption') continue;
    const style = { ...item.props.style };
    if (change.fontSize !== undefined) style.fontSize = change.fontSize;
    else if (change.sizeFactor !== undefined) style.fontSize = Math.max(8, Math.round(style.fontSize * change.sizeFactor));
    if (change.color) style.color = change.color;
    if (change.placementY !== undefined) style.placementY = change.placementY;
    ops.push({ type: 'item.update', itemId: item.id, patch: { props: { ...item.props, style } } as never });
  }
  return ops.length > 0 ? [{ type: 'batch', ops } as Op] : [];
}
