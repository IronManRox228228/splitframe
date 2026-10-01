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
  assets: { id: string; kind: string }[];
  transcripts: { assetId: string; words: TranscriptWord[] }[];
}

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

/** Cut [startFrame, endFrame) spans out of one item via split+remove, right-to-left. */
function cutSpansOps(item: Item, spans: { startFrame: number; endFrame: number }[]): Op[] {
  const ops: Op[] = [];
  const sorted = [...spans]
    .filter((s) => s.endFrame > s.startFrame)
    .sort((a, b) => b.startFrame - a.startFrame); // right-to-left: earlier coords stay valid
  const originalEnd = itemEnd(item);
  let currentId = item.id; // the left part accumulates every cut
  for (const span of sorted) {
    const atStart = Math.max(item.startFrame + 1, span.startFrame);
    if (atStart >= originalEnd) continue;
    const mid = newId('itm');
    ops.push({ type: 'item.split', itemId: currentId, atFrame: atStart, newItemId: mid });
    if (span.endFrame < originalEnd) {
      const right = newId('itm');
      ops.push({ type: 'item.split', itemId: mid, atFrame: span.endFrame, newItemId: right });
    }
    // mid now covers exactly [span.start, span.end) (or to the end) — drop it, ripple closes
    ops.push({ type: 'item.remove', itemIds: [mid], ripple: true });
  }
  return ops;
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
        ops.push({
          type: 'item.add',
          item: {
            id: newId('itm'),
            trackId: track.id,
            type: 'caption',
            startFrame: start,
            durationFrames: end - start,
            transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
            props: { words: card.map((w) => ({ w: w.w, startMs: w.startMs, endMs: w.endMs })), style, mode: 'phrase', maxWordsPerCard: input.wordsPerCard },
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
    'Cut silent gaps out of speech items using the transcript timing (default: pauses over 0.5s). Applied atomically with ripple so downstream items slide closed. Use thresholdSec 0.3 for tight cuts, 0.8 for a relaxed feel.',
  input: z.object({
    itemIds: z.array(z.string()).optional(),
    thresholdSec: z.number().min(0.1).max(5).default(0.5),
    keepLeading: z.boolean().default(false).describe('Keep silence before the first word'),
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
      const words = wordsForItem(snapshot, item);
      if (words.length < 2) continue;
      const spans: { startFrame: number; endFrame: number }[] = [];
      const thresholdFrames = Math.round(input.thresholdSec * fps);
      if (!input.keepLeading && words[0]!.startFrame - item.startFrame > thresholdFrames) {
        spans.push({ startFrame: item.startFrame + 1, endFrame: words[0]!.startFrame });
      }
      for (let i = 0; i < words.length - 1; i++) {
        const gap = words[i + 1]!.startFrame - words[i]!.endFrame;
        if (gap > thresholdFrames) {
          spans.push({ startFrame: words[i]!.endFrame, endFrame: words[i + 1]!.startFrame });
        }
      }
      const lastWord = words[words.length - 1]!;
      if (itemEnd(item) - lastWord.endFrame > thresholdFrames) {
        spans.push({ startFrame: lastWord.endFrame, endFrame: itemEnd(item) });
      }
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
        spans.push({ startFrame: words[i]!.startFrame, endFrame: words[i + minWords - 1]!.endFrame, text: phrase });
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
      const words = wordsForItem(snapshot, item);
      const spans: { startFrame: number; endFrame: number }[] = [];
      // pauses
      const thresholdFrames = Math.round(input.thresholdSec * fps);
      for (let i = 0; i < words.length - 1; i++) {
        if (words[i + 1]!.startFrame - words[i]!.endFrame > thresholdFrames) {
          spans.push({ startFrame: words[i]!.endFrame, endFrame: words[i + 1]!.startFrame });
        }
      }
      // retakes (drop earlier repeats)
      for (const retake of findRetakeSpans(snapshot, item, input.retakeMinWords)) {
        spans.push({ startFrame: retake.startFrame, endFrame: retake.endFrame });
        retakes++;
      }
      ops.push(...cutSpansOps(item, spans));
    }
    await ctx.applyOps(ops, ctx.actor, `buildRoughCut (${retakes} retakes)`);
    const after = await ctx.getSnapshot();
    return { applied: true, retakesRemoved: retakes, duration: timelineDuration(after) };
  },
};

// ---------- duckMusic ----------

export const duckMusic: ToolDef = {
  name: 'duckMusic',
  description:
    'Duck music under speech: adds volume keyframes on the music item (default 25% under speech, 150ms ramps), computed from the speech items\' transcripts. Also honored by the exporter.',
  input: z.object({
    musicItemId: z.string().describe('The audio item to duck'),
    level: z.number().min(0).max(1).default(0.25),
    rampMs: z.number().min(0).max(1000).default(150),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const music = snapshot.doc.items.find((i) => i.id === input.musicItemId);
    if (!music) throw new Error(`Item ${input.musicItemId} not found. Call getTimeline.`);
    if (music.type !== 'audio') throw new Error('duckMusic targets an audio item.');
    const fps = snapshot.doc.project.fps;
    const ramp = Math.round((input.rampMs / 1000) * fps);

    const spans: { start: number; end: number }[] = [];
    for (const item of speechItems(snapshot)) {
      const words = wordsForItem(snapshot, item);
      if (words.length === 0) continue;
      const spanStart = Math.max(music.startFrame, words[0]!.startFrame - ramp);
      const spanEnd = Math.min(itemEnd(music), words[words.length - 1]!.endFrame + ramp);
      // merge close spans (< 2 * ramp apart)
      if (spans.length > 0 && spanStart - spans[spans.length - 1]!.end < ramp * 2) {
        spans[spans.length - 1]!.end = spanEnd;
      } else {
        spans.push({ start: spanStart, end: spanEnd });
      }
    }
    if (spans.length === 0) throw new Error('No speech found to duck under.');

    const keyframes: { frame: number; value: number; easing: 'linear' }[] = [];
    for (const span of spans.slice(0, 40)) {
      keyframes.push(
        { frame: Math.max(0, span.start - ramp), value: 1, easing: 'linear' },
        { frame: span.start, value: input.level, easing: 'linear' },
        { frame: span.end, value: input.level, easing: 'linear' },
        { frame: span.end + ramp, value: 1, easing: 'linear' },
      );
    }
    await ctx.applyOps(
      [{ type: 'item.setKeyframes', itemId: music.id, property: 'volume', keyframes }],
      ctx.actor,
      'duckMusic',
    );
    return { applied: true, duckedSpans: spans.length, level: input.level };
  },
};

export const MACRO_TOOLS: ToolDef[] = [addCaptions, removeSilences, buildRoughCut, duckMusic];

// re-export for tests
export { wordsForItem, cutSpansOps, sourceFrameAt, itemsOnTrack };
