import { z } from 'zod';
import { newId, type Item, type Op, type TranscriptWord } from '@cutboard/schema';
import { itemEnd, itemsOnTrack, sourceFrameAt } from '@cutboard/editor-core';
import type { ToolDef, ToolContext } from '../registry.ts';

/**
 * Macro tools (main prompt §6): thin wrappers over the primitive ops so weaker models
 * succeed. All of them are composite op lists applied atomically via batchEdit-style
 * commits, so one undo undoes the whole macro.
 */

interface Snapshot {
  doc: {
    project: { fps: number };
    tracks: { id: string; kind: string; name: string; locked: boolean }[];
    items: Item[];
    markers: unknown[];
  };
  assets: { id: string; kind: string; hasSpeech?: boolean; durationMs?: number }[];
  transcripts: { assetId: string; words: TranscriptWord[] }[];
}

type Silence = { startMs: number; endMs: number };

function requireSnapshot(snap: unknown): Snapshot {
  const s = snap as Snapshot;
  if (!s || typeof s !== 'object' || !('doc' in s)) {
    throw new Error('No project open. Open a project first.');
  }
  return s;
}

/** Timeline words for an item: transcript words mapped into item-local timeline frames. */
function wordsForItem(snapshot: Snapshot, item: Item): { w: string; startFrame: number; endFrame: number; startMs: number; endMs: number }[] {
  const fps = snapshot.doc.project.fps;
  const transcript = snapshot.transcripts.find((t) => t.assetId === item.assetId);
  if (!transcript) return [];
  const speed = item.speed;
  const out: { w: string; startFrame: number; endFrame: number; startMs: number; endMs: number }[] = [];
  for (const word of transcript.words) {
    const sourceFrameLocal = Math.round((word.startMs / 1000) * fps);
    const timeline = item.startFrame + (sourceFrameLocal - (item.sourceInFrame ?? 0)) / speed;
    const durFrames = Math.max(1, Math.round(((word.endMs - word.startMs) / 1000) * fps) / speed);
    out.push({
      w: word.w,
      startFrame: Math.round(timeline),
      endFrame: Math.round(timeline + durFrames),
      startMs: word.startMs,
      endMs: word.endMs,
    });
  }
  return out.filter((w) => w.endFrame > item.startFrame && w.startFrame < itemEnd(item));
}

function speechItems(snapshot: Snapshot, itemIds?: string[]): Item[] {
  const items = snapshot.doc.items.filter(
    (i) => (itemIds ? itemIds.includes(i.id) : true) && (i.type === 'video' || i.type === 'audio'),
  );
  return items.filter((i) => snapshot.transcripts.some((t) => t.assetId === i.assetId && t.words.length > 0));
}

/** Sort spans and merge any that overlap or touch, so cuts never double-cover frames. */
function mergeSpans(spans: { startFrame: number; endFrame: number }[]): { startFrame: number; endFrame: number }[] {
  const sorted = spans.filter((s) => s.endFrame > s.startFrame).sort((a, b) => a.startFrame - b.startFrame);
  const merged: { startFrame: number; endFrame: number }[] = [];
  for (const span of sorted) {
    const last = merged[merged.length - 1];
    if (last && span.startFrame <= last.endFrame) last.endFrame = Math.max(last.endFrame, span.endFrame);
    else merged.push({ ...span });
  }
  return merged;
}

/** Cut [startFrame, endFrame) spans out of one item via split+remove, right-to-left. */
function cutSpansOps(item: Item, spans: { startFrame: number; endFrame: number }[]): Op[] {
  const ops: Op[] = [];
  const sorted = mergeSpans(spans).reverse(); // right-to-left: earlier coords stay valid
  let currentId = item.id; // the left part accumulates every cut
  let currentEnd = itemEnd(item); // end of that left part (cuts only ever remove from its right)
  for (const span of sorted) {
    const spanStart = Math.max(item.startFrame, span.startFrame);
    const spanEnd = Math.min(currentEnd, span.endFrame);
    if (spanEnd <= spanStart) continue;
    // a span that starts at the item's first frame consumes the whole remaining left part
    let mid = currentId;
    if (spanStart > item.startFrame) {
      mid = newId('itm');
      ops.push({ type: 'item.split', itemId: currentId, atFrame: spanStart, newItemId: mid });
    }
    if (spanEnd < currentEnd) {
      ops.push({ type: 'item.split', itemId: mid, atFrame: spanEnd, newItemId: newId('itm') });
    }
    // mid now covers exactly [spanStart, spanEnd) — drop it, ripple closes the gap
    ops.push({ type: 'item.remove', itemIds: [mid], ripple: true });
    currentEnd = spanStart;
  }
  return ops;
}

// ---------- pauses & speech runs (from the audio, not from ASR word gaps) ----------

/** Whisper stretches a word's end time across the pause that follows it, so word gaps hide pauses.
 * The real pauses come from the audio's silence map; word starts are only used to clean up cut points. */
async function silencesOf(ctx: ToolContext, item: Item): Promise<Silence[] | null> {
  if (!item.assetId || !ctx.getSilences) return null;
  try {
    return await ctx.getSilences(item.assetId);
  } catch {
    return null; // detection failed: fall back to transcript gaps
  }
}

/** Timeline frame of a source time in ms for this item. */
function msToTimelineFrame(item: Item, fps: number, ms: number): number {
  return Math.round(item.startFrame + ((ms / 1000) * fps - (item.sourceInFrame ?? 0)) / item.speed);
}

/**
 * Timeline spans of an item to cut as pauses: silences longer than `thresholdSec`, shrunk by a
 * `handleSec` handle on each side so no word is clipped.
 * Without a silence map, transcript word gaps are used (they under-report). `keepLeading` leaves silence before the first word.
 */
export function pauseSpans(
  snapshot: Snapshot,
  item: Item,
  silences: Silence[] | null,
  opts: { thresholdSec: number; keepLeading: boolean; handleSec: number },
): { startFrame: number; endFrame: number }[] {
  const fps = snapshot.doc.project.fps;
  const words = wordsForItem(snapshot, item);
  const thresholdFrames = Math.round(opts.thresholdSec * fps);
  const spans: { startFrame: number; endFrame: number }[] = [];
  const transcript = snapshot.transcripts.find((t) => t.assetId === item.assetId);
  const firstWordMs = transcript?.words[0]?.startMs ?? Infinity;

  if (silences) {
    const handleMs = opts.handleSec * 1000;
    const itemStartMs = ((item.sourceInFrame ?? 0) / fps) * 1000;
    const itemEndMs = itemStartMs + ((item.durationFrames * item.speed) / fps) * 1000;
    for (const sil of silences) {
      if ((sil.endMs - sil.startMs) / 1000 <= opts.thresholdSec) continue;
      if (opts.keepLeading && sil.endMs <= firstWordMs + 1) continue;
      // ASR word times are stretched across pauses at BOTH ends (the last word of a sentence can start
      // in the middle of the silence), so the audio decides where speech stops and starts
      const start0 = sil.startMs;
      let end = sil.endMs;
      // keep a handle next to speech; at the very start / end of the item there is no speech to protect
      const start = start0 <= itemStartMs + 20 ? start0 : start0 + handleMs;
      if (!(end >= itemEndMs - 20)) end -= handleMs;
      if (end <= start) continue;
      spans.push({
        startFrame: Math.max(item.startFrame, msToTimelineFrame(item, fps, start)),
        endFrame: Math.min(itemEnd(item), msToTimelineFrame(item, fps, end)),
      });
    }
  }

  // transcript gaps (also the only source when there is no silence map)
  if (!silences && !opts.keepLeading && words.length > 0 && words[0]!.startFrame - item.startFrame > thresholdFrames) {
    spans.push({ startFrame: item.startFrame, endFrame: words[0]!.startFrame });
  }
  for (let i = 0; !silences && i < words.length - 1; i++) {
    if (words[i + 1]!.startFrame - words[i]!.endFrame > thresholdFrames) {
      spans.push({ startFrame: words[i]!.endFrame, endFrame: words[i + 1]!.startFrame });
    }
  }
  if (!silences && words.length > 0 && itemEnd(item) - words[words.length - 1]!.endFrame > thresholdFrames) {
    spans.push({ startFrame: words[words.length - 1]!.endFrame, endFrame: itemEnd(item) });
  }
  return spans.filter((s) => s.endFrame > s.startFrame);
}

// ---------- addCaptions ----------

const CAPTION_PRESETS = {
  serif: { fontFamily: 'Georgia', fontSize: 64, fontWeight: 700, color: '#ffffff', strokeWidth: 6, strokeColor: '#000000', highlight: 'none' as const, uppercase: false },
  bold: { fontFamily: 'Inter', fontSize: 72, fontWeight: 900, color: '#ffffff', strokeWidth: 0, highlight: 'none' as const, uppercase: true },
  karaoke: { fontFamily: 'Inter', fontSize: 76, fontWeight: 900, color: '#ffffff', strokeWidth: 8, strokeColor: '#000000', highlight: 'active-word' as const, uppercase: true },
};

export const addCaptions: ToolDef = {
  name: 'addCaptions',
  description:
    'Generate captions from the transcript for speech items (or specific itemIds). Presets: "serif", "bold", "karaoke" (active-word highlight). Word timings come from ASR, so they match the audio. Pass style overrides to fine-tune.',
  input: z.object({
    itemIds: z.array(z.string()).optional().describe('Speech items to caption; default = all with transcripts'),
    preset: z.enum(['serif', 'bold', 'karaoke']).default('karaoke'),
    wordsPerCard: z.number().int().min(1).max(12).default(4),
    fontSize: z.number().positive().optional(),
    color: z.string().optional(),
    placementY: z.number().min(0).max(1).optional().describe('0 = top of frame, 1 = bottom'),
    trackId: z.string().optional().describe('Caption track; default = first text track'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const fps = snapshot.doc.project.fps;
    const items = speechItems(snapshot, input.itemIds);
    if (items.length === 0) {
      throw new Error('No speech items with transcripts found. Run transcription first (import audio-bearing footage), or pass itemIds.');
    }
    const track =
      (input.trackId ? snapshot.doc.tracks.find((t) => t.id === input.trackId) : undefined) ??
      snapshot.doc.tracks.find((t) => t.kind === 'text');
    if (!track) throw new Error('No text track available.');
    if (track.locked) throw new Error(`Track "${track.name}" is locked.`);

    const preset = CAPTION_PRESETS[input.preset as keyof typeof CAPTION_PRESETS];
    const style = {
      ...preset,
      fontSize: input.fontSize ?? preset.fontSize,
      color: input.color ?? preset.color,
      placementY: input.placementY ?? 0.82,
      align: 'center' as const,
      lineHeight: 1.2,
      padding: 8,
      maxCharsPerLine: 32,
    };
    const pad = Math.round(0.1 * fps);

    const ops: Op[] = [];
    let count = 0;
    for (const item of items) {
      const words = wordsForItem(snapshot, item);
      if (words.length === 0) continue;
      for (let i = 0; i < words.length; i += input.wordsPerCard) {
        const card = words.slice(i, i + input.wordsPerCard);
        const start = Math.max(item.startFrame, card[0]!.startFrame);
        const nextCard = words[i + input.wordsPerCard];
        const end = Math.min(itemEnd(item), Math.max(card[card.length - 1]!.endFrame, nextCard ? nextCard.startFrame - 1 : card[card.length - 1]!.endFrame + pad));
        if (end - start < 1) continue;
        // word times are stored relative to the caption's own start, so they stay valid
        // when the caption moves and no matter where the source clip sits on the timeline
        const msFromCardStart = (frame: number) => Math.max(0, Math.round(((frame - start) / fps) * 1000));
        ops.push({
          type: 'item.add',
          item: {
            id: newId('itm'),
            trackId: track.id,
            type: 'caption',
            startFrame: start,
            durationFrames: end - start,
            transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
            props: { words: card.map((w) => ({ w: w.w, startMs: msFromCardStart(w.startFrame), endMs: msFromCardStart(w.endFrame) })), style, mode: 'phrase', maxWordsPerCard: input.wordsPerCard },
            labels: { name: card.map((w) => w.w).join(' ').slice(0, 24) },
          } as never,
        });
        count++;
      }
    }
    if (ops.length === 0) throw new Error('No caption cards generated (empty transcripts?).');
    await ctx.applyOps(ops, ctx.actor, `addCaptions (${count} cards)`);
    return { applied: true, captionCards: count, preset: input.preset, note: 'Verify with getTimeline and captureFrame.' };
  },
};

// ---------- removeSilences ----------

export const removeSilences: ToolDef = {
  name: 'removeSilences',
  description:
    'Cut pauses out of speech items, found from the audio itself (default: pauses over 0.5s), keeping a short handle so words are not clipped. Applied atomically with ripple so downstream items slide closed. thresholdSec 0.3 = tight cuts, 0.8 = relaxed.',
  input: z.object({
    itemIds: z.array(z.string()).optional(),
    thresholdSec: z.number().min(0.1).max(5).default(0.5),
    keepLeading: z.boolean().default(false).describe('Keep silence before the first word'),
    handleSec: z.number().min(0).max(0.5).default(0.1).describe('Silence kept next to speech on each side of a cut'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const fps = snapshot.doc.project.fps;
    const items = speechItems(snapshot, input.itemIds).sort((a, b) => b.startFrame - a.startFrame);
    if (items.length === 0) throw new Error('No speech items with transcripts found.');
    const ops: Op[] = [];
    let cutFrames = 0;
    for (const item of items) {
      const spans = mergeSpans(pauseSpans(snapshot, item, await silencesOf(ctx, item), input));
      for (const span of spans) cutFrames += span.endFrame - span.startFrame;
      ops.push(...cutSpansOps(item, spans));
    }
    if (ops.length === 0) throw new Error('No silences over the threshold found.');
    await ctx.applyOps(ops, ctx.actor, `removeSilences (${(cutFrames / fps).toFixed(1)}s cut)`);
    const after = await ctx.getSnapshot();
    return {
      applied: true,
      cutSeconds: Number((cutFrames / fps).toFixed(2)),
      note: 'Pauses removed atomically. Verify with getTimeline + getTimelineDuration.',
      duration: timelineDuration(after),
    };
  },
};

function timelineDuration(snapshot: unknown): { frames: number; seconds: number } {
  const s = snapshot as Snapshot;
  const frames = s.doc.items.reduce((max, i) => Math.max(max, i.startFrame + i.durationFrames), 0);
  return { frames, seconds: Number((frames / s.doc.project.fps).toFixed(2)) };
}

// ---------- buildRoughCut ----------

const MAX_RETAKE_WORDS = 20;

function findRetakeSpans(snapshot: Snapshot, item: Item, minWords: number): { startFrame: number; endFrame: number; text: string }[] {
  const words = wordsForItem(snapshot, item);
  const n = words.length;
  if (n < minWords * 2) return [];
  const spans: { startFrame: number; endFrame: number; text: string }[] = [];
  const key = (w: { w: string }) => w.w.toLowerCase().replace(/[^a-z0-9']/g, '');
  for (let i = 0; i + minWords <= n; i++) {
    const phrase = words.slice(i, i + minWords).map(key).join(' ');
    if (phrase.split(' ').some((x) => !x)) continue;
    // find a later repeat of this phrase
    for (let j = i + minWords; j + minWords <= n; j++) {
      const candidate = words.slice(j, j + minWords).map(key).join(' ');
      if (candidate === phrase) {
        // a short false start ("So the reason we started this company was, well ... So the reason we ...") goes
        // whole, up to where the kept take begins; a long gap means the phrase just recurs, so cut only the phrase
        const whole = j - i <= MAX_RETAKE_WORDS;
        spans.push({ startFrame: words[i]!.startFrame, endFrame: whole ? words[j]!.startFrame : words[i + minWords - 1]!.endFrame, text: phrase });
        i = j; // skip past the kept occurrence
        break;
      }
    }
  }
  return spans;
}

export const buildRoughCut: ToolDef = {
  name: 'buildRoughCut',
  description:
    'Rough cut: remove pauses over a threshold and drop repeated takes (same phrase said twice — keeps the LAST, usually cleanest attempt). Keep the user\'s order. Reports the resulting duration; combine with addCaptions next.',
  input: z.object({
    itemIds: z.array(z.string()).optional().describe('Speech items; default = all with transcripts'),
    thresholdSec: z.number().min(0.1).max(5).default(0.5),
    retakeMinWords: z.number().int().min(3).max(12).default(5).describe('Min words in a repeated phrase to count as a retake'),
    handleSec: z.number().min(0).max(0.5).default(0.1).describe('Silence kept next to speech on each side of a cut'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const fps = snapshot.doc.project.fps;
    const items = speechItems(snapshot, input.itemIds).sort((a, b) => b.startFrame - a.startFrame);
    if (items.length === 0) throw new Error('No speech items with transcripts found.');
    const ops: Op[] = [];
    let retakes = 0;
    for (const item of items) {
      // pauses, measured from the audio
      const spans = pauseSpans(snapshot, item, await silencesOf(ctx, item), { thresholdSec: input.thresholdSec, keepLeading: false, handleSec: input.handleSec });
      // retakes (drop earlier repeats)
      for (const retake of findRetakeSpans(snapshot, item, input.retakeMinWords)) {
        spans.push({ startFrame: retake.startFrame, endFrame: retake.endFrame });
        retakes++;
      }
      ops.push(...cutSpansOps(item, spans));
    }
    if (ops.length === 0) throw new Error('Nothing to cut: no pauses over the threshold and no repeated takes found.');
    await ctx.applyOps(ops, ctx.actor, `buildRoughCut (${retakes} retakes)`);
    const after = await ctx.getSnapshot();
    return { applied: true, retakesRemoved: retakes, duration: timelineDuration(after) };
  },
};

// ---------- duckMusic ----------

const NON_WORD = /^\W*$|^\[.*\]$|^[♪♫]+$/u;

/** Real speech has words at a talking pace; Whisper on music yields a few stray or looping tokens. */
export function looksLikeSpeech(words: TranscriptWord[], durationMs: number): boolean {
  const real = words.filter((w) => !NON_WORD.test(w.w.trim()));
  if (real.length < 3) return false;
  const spanSec = Math.max(durationMs, real[real.length - 1]!.endMs) / 1000;
  if (real.length / spanSec < 0.5) return false;
  if (real.length >= 8) {
    const unique = new Set(real.map((w) => w.w.toLowerCase().replace(/[^\p{L}\p{N}']/gu, '')));
    if (unique.size / real.length < 0.3) return false;
  }
  return true;
}

/** Speech runs [startMs, endMs] in source time: the audio between pauses of at least gapMs that holds words. */
export function speechRunsMs(words: TranscriptWord[], silences: Silence[] | null, rangeMs: [number, number], gapMs: number): [number, number][] {
  const inRange = words.filter((w) => w.endMs > rangeMs[0] && w.startMs < rangeMs[1]);
  if (inRange.length === 0) return [];
  if (!silences) {
    const runs: [number, number][] = [];
    for (const w of inRange) {
      const last = runs[runs.length - 1];
      if (last && w.startMs - last[1] < gapMs) last[1] = Math.max(last[1], w.endMs);
      else runs.push([w.startMs, w.endMs]);
    }
    return runs;
  }
  const quiet = silences.filter((s) => s.endMs - s.startMs >= gapMs).sort((a, b) => a.startMs - b.startMs);
  const runs: [number, number][] = [];
  let cursor = rangeMs[0];
  for (const s of quiet) {
    if (s.startMs > cursor) runs.push([cursor, Math.min(s.startMs, rangeMs[1])]);
    cursor = Math.max(cursor, s.endMs);
  }
  if (cursor < rangeMs[1]) runs.push([cursor, rangeMs[1]]);
  // keep only runs that contain a word start (drops noise between pauses)
  return runs.filter(([a, b]) => b > a && inRange.some((w) => w.startMs >= a - 50 && w.startMs < b));
}

export const duckMusic: ToolDef = {
  name: 'duckMusic',
  description:
    "Duck music under speech: volume keyframes on the music item, lowered to level x its own volume while someone talks and back up in pauses longer than gapSec. Speech is found from the voice items' audio and transcripts; music is never treated as speech. Pass speechItemIds to name the voice items explicitly.",
  input: z.object({
    musicItemId: z.string().describe('The audio item to duck'),
    level: z.number().min(0).max(1).default(0.25).describe("Ducked volume as a fraction of the item's own volume"),
    rampMs: z.number().min(0).max(1000).default(150),
    gapSec: z.number().min(0.1).max(5).default(0.5).describe('Pauses at least this long let the music come back up'),
    speechItemIds: z.array(z.string()).optional().describe('Voice items to duck under; default = detected from transcripts'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const music = snapshot.doc.items.find((i) => i.id === input.musicItemId);
    if (!music) throw new Error(`Item ${input.musicItemId} not found. Call getTimeline.`);
    if (music.type !== 'audio') throw new Error('duckMusic targets an audio item.');
    const fps = snapshot.doc.project.fps;
    const ramp = Math.round((input.rampMs / 1000) * fps);

    const sources = speechItems(snapshot, input.speechItemIds).filter((i) => {
      if (i.id === music.id || i.assetId === music.assetId) return false; // the music itself is never speech
      if (input.speechItemIds) return true; // the caller named it
      const asset = snapshot.assets.find((a) => a.id === i.assetId);
      if (asset?.hasSpeech === false) return false;
      const words = snapshot.transcripts.find((t) => t.assetId === i.assetId)?.words ?? [];
      return looksLikeSpeech(words, asset?.durationMs ?? 0);
    });

    // speech runs in music-local frames
    const runs: { start: number; end: number }[] = [];
    for (const item of sources) {
      const words = snapshot.transcripts.find((t) => t.assetId === item.assetId)?.words ?? [];
      const srcStartMs = ((item.sourceInFrame ?? 0) / fps) * 1000;
      const srcEndMs = srcStartMs + ((item.durationFrames * item.speed) / fps) * 1000;
      for (const [a, b] of speechRunsMs(words, await silencesOf(ctx, item), [srcStartMs, srcEndMs], input.gapSec * 1000)) {
        runs.push({
          start: msToTimelineFrame(item, fps, Math.max(a, srcStartMs)) - music.startFrame,
          end: msToTimelineFrame(item, fps, Math.min(b, srcEndMs)) - music.startFrame,
        });
      }
    }
    const merged: { start: number; end: number }[] = [];
    for (const r of runs.sort((a, b) => a.start - b.start)) {
      const last = merged[merged.length - 1];
      // runs closer than the gap threshold (or the two ramps) are one run
      if (last && r.start - last.end < Math.max(2 * ramp, Math.round(input.gapSec * fps))) last.end = Math.max(last.end, r.end);
      else merged.push({ ...r });
    }
    const clipped = merged.filter((r) => r.end > 0 && r.start < music.durationFrames);
    if (clipped.length === 0) {
      throw new Error('No speech found over the music to duck under (music is never treated as speech; pass speechItemIds to name voice items).');
    }

    // the envelope is relative to the item's own volume, and keyframe frames are item-local
    const hi = music.volume;
    const lo = hi * input.level;
    const keyframes: { frame: number; value: number; easing: 'linear' }[] = [];
    const push = (frame: number, value: number) => {
      if (frame > music.durationFrames) return;
      const f = Math.max(0, frame);
      const last = keyframes[keyframes.length - 1];
      if (last && f <= last.frame) {
        if (f === last.frame) last.value = value;
        return;
      }
      keyframes.push({ frame: f, value, easing: 'linear' });
    };
    const spans = clipped.slice(0, 100);
    for (const r of spans) {
      push(r.start - ramp, hi);
      push(r.start, lo);
      push(r.end, lo);
      push(r.end + ramp, hi);
    }
    await ctx.applyOps([{ type: 'item.setKeyframes', itemId: music.id, property: 'volume', keyframes }], ctx.actor, 'duckMusic');
    return { applied: true, duckedSpans: spans.length, level: input.level, volumeUnder: Number(lo.toFixed(3)), volumeClear: hi, speechItems: sources.length };
  },
};

export const MACRO_TOOLS: ToolDef[] = [addCaptions, removeSilences, buildRoughCut, duckMusic];

// re-export for tests
export { wordsForItem, cutSpansOps, sourceFrameAt, itemsOnTrack, findRetakeSpans, mergeSpans };
