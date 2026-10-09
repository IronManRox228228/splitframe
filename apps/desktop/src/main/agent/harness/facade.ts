import { z } from 'zod';
import { newId, type Item, type Op } from '@cutboard/schema';
import { cutSpansOps, looksLikeSpeech, mergeSpans } from '@cutboard/tools';
import {
  assembleOps,
  captionStyleOps,
  cutSourceRangesOps,
  fillerRanges,
  layoutInOrderOps,
  reorderedItems,
  retakeOps,
  sentencesOf,
  setClipRangeOps,
  snapToSpeech,
  DEFAULT_FILLERS,
} from './compiler.ts';
import { buildHandles, resolveAsset, resolveClip, resolveClips, type HandleEntry } from './handles.ts';
import type { Backend, Finding, Snapshot } from './types.ts';
import { fmtDur, fmtTime, toFrames } from './units.ts';
import * as V from './verify.ts';

/**
 * The model-facing tool facade. Thin tools over the compiler and the existing registry: seconds
 * instead of frames, clip handles / names instead of ids, defaults supplied by code, and a
 * verification of the result before the tool returns. The classic tool layer (MCP, classic chat)
 * is untouched; nothing here changes its names or schemas.
 */

export interface Outcome {
  ok: boolean;
  /** verified facts (what the timeline now says), or the reason it failed; shown to the model and the user */
  summary: string;
  findings: Finding[];
  mutated: boolean;
  /** the user declined the change (Ask mode / a confirmation); nothing was done and nothing should be retried */
  skipped?: boolean;
}

export interface FacadeTool {
  name: string;
  description: string;
  input: z.ZodObject;
  mutates: boolean;
  /** what the call would do beyond editing the timeline: the modes always ask before these */
  risk?(args: Record<string, unknown>): { removesMedia?: boolean; costUsd?: number };
  run(args: Record<string, unknown>, backend: Backend): Promise<Outcome>;
}

const bad = (summary: string): Outcome => ({ ok: false, summary, findings: [{ level: 'fail', message: summary }], mutated: false });
const done = (summary: string, findings: Finding[] = [], mutated = true): Outcome => ({ ok: !V.hasFailure(findings), summary, findings, mutated });

const time = z.union([z.number(), z.string()]);
const sec = (v: unknown): number | null => {
  const n = typeof v === 'number' ? v : typeof v === 'string' ? Number(v) : NaN;
  if (Number.isFinite(n) && n >= 0) return n;
  return typeof v === 'string' ? parseClock(v) : null;
};
function parseClock(s: string): number | null {
  const m = /^(\d+):(\d{1,2}(?:\.\d+)?)$/.exec(s.trim());
  return m ? Number(m[1]) * 60 + Number(m[2]) : null;
}

/** Make a tool from a typed run function; schema failures and thrown errors become failed outcomes. */
function tool<S extends z.ZodObject>(def: {
  name: string;
  description: string;
  input: S;
  mutates: boolean;
  run(args: z.infer<S>, ctx: Ctx): Promise<Outcome>;
}): FacadeTool {
  return {
    name: def.name,
    description: def.description,
    input: def.input,
    mutates: def.mutates,
    async run(raw, backend) {
      const parsed = def.input.safeParse(raw);
      if (!parsed.success) return bad(`Bad arguments for ${def.name}: ${parsed.error.issues.map((i) => `${i.path.join('.') || 'args'} ${i.message}`).join('; ')}`);
      try {
        const snap = await backend.snapshot();
        return await def.run(parsed.data, { backend, snap, entries: buildHandles(snap), fps: snap.doc.project.fps });
      } catch (err) {
        return bad(err instanceof Error ? err.message : String(err));
      }
    },
  };
}

interface Ctx {
  backend: Backend;
  snap: Snapshot;
  entries: HandleEntry[];
  fps: number;
}

const lengthLine = (before: Snapshot, after: Snapshot): string => {
  const b = V.docLengthFrames(before.doc) / before.doc.project.fps;
  const a = V.docLengthFrames(after.doc) / after.doc.project.fps;
  return Math.abs(a - b) < 0.05 ? `length stays ${fmtTime(a)}` : `length ${fmtTime(b)} -> ${fmtTime(a)}`;
};

/** Transcript-bearing speech assets on the timeline (music transcribed by Whisper is not speech). */
export function speechAssetIds(snap: Snapshot, clips?: Item[]): string[] {
  const pool = clips ?? snap.doc.items.filter((i) => i.type === 'video' || i.type === 'audio');
  const ids = new Set<string>();
  for (const item of pool) {
    const asset = snap.assets.find((a) => a.id === item.assetId);
    const words = snap.transcripts.find((t) => t.assetId === item.assetId)?.words ?? [];
    if (asset && words.length > 0 && looksLikeSpeech(words, asset.durationMs)) ids.add(asset.id);
  }
  return [...ids];
}

function speechClips(c: Ctx, refs?: string[]): { items: Item[] } | { error: string } {
  if (refs && refs.length > 0) {
    const r = resolveClips(c.entries, c.snap.editor, refs);
    return r.ok ? { items: r.value.map((e) => e.item) } : { error: r.error };
  }
  const ids = new Set(speechAssetIds(c.snap));
  const items = c.snap.doc.items.filter((i) => (i.type === 'video' || i.type === 'audio') && i.assetId && ids.has(i.assetId));
  return items.length > 0 ? { items } : { error: 'No clip on the timeline has speech with a transcript.' };
}

const clipArg = z.string().describe('Clip handle like "V1·2" (from the timeline listing), its file name, or "selected"');

// ---------------------------------------------------------------- clip editing

export const trimClip = tool({
  name: 'trimClip',
  description:
    'Keep only part of a clip: seconds from..to of the ORIGINAL media (the "src" range in the timeline listing); everything outside from..to is dropped. To keep just the opening N seconds: from 0 to N. To drop the first N seconds: from N to the clip\'s end. To cut a stretch OUT of the middle, use removeSection instead. The clip stays where it is; later clips close up.',
  input: z.object({ clip: clipArg, from: time.describe('start, seconds in the source'), to: time.describe('end, seconds in the source'), ripple: z.boolean().default(true) }),
  mutates: true,
  async run(a, c) {
    const r = resolveClip(c.entries, c.snap.editor, a.clip);
    if (!r.ok) return bad(r.error);
    const from = sec(a.from);
    const to = sec(a.to);
    if (from === null || to === null) return bad('from and to must be seconds, e.g. 0 and 4.');
    const { ops, newDurationFrames } = setClipRangeOps(c.snap, r.value.item, from, to, a.ripple);
    await c.backend.apply(ops, 'Trim clip');
    const after = await c.backend.snapshot();
    const f = [...V.itemRange(after.doc, r.value.item.id, r.value.item.startFrame, newDurationFrames), ...V.newOverlaps(c.snap.doc, after.doc)];
    return done(`${r.value.handle} "${r.value.name}" now plays source ${fmtTime(from)}–${fmtTime(to)} (${fmtDur(newDurationFrames / c.fps)}); ${lengthLine(c.snap, after)}`, f);
  },
});

export const splitClip = tool({
  name: 'splitClip',
  description: 'Split a clip in two at a point on the TIMELINE (seconds from the start of the video).',
  input: z.object({ clip: clipArg, atSec: time.describe('timeline seconds') }),
  mutates: true,
  async run(a, c) {
    const r = resolveClip(c.entries, c.snap.editor, a.clip);
    if (!r.ok) return bad(r.error);
    const at = sec(a.atSec);
    if (at === null) return bad('atSec must be seconds.');
    const frame = toFrames(at, c.fps);
    const i = r.value.item;
    if (frame <= i.startFrame || frame >= i.startFrame + i.durationFrames) {
      return bad(`${r.value.handle} runs ${fmtTime(i.startFrame / c.fps)}–${fmtTime((i.startFrame + i.durationFrames) / c.fps)}; ${fmtTime(at)} is outside it.`);
    }
    await c.backend.apply([{ type: 'item.split', itemId: i.id, atFrame: frame, newItemId: newId('itm') }], 'Split clip');
    const after = await c.backend.snapshot();
    const f = after.doc.items.length === c.snap.doc.items.length + 1 ? [] : [{ level: 'fail' as const, message: 'the clip was not split' }];
    return done(`split ${r.value.handle} "${r.value.name}" at ${fmtTime(at)} into two clips`, f);
  },
});

export const deleteClips = tool({
  name: 'deleteClips',
  description: 'Delete one or more clips. By default the gap closes (ripple).',
  input: z.object({ clips: z.array(clipArg).min(1), ripple: z.boolean().default(true) }),
  mutates: true,
  async run(a, c) {
    const r = resolveClips(c.entries, c.snap.editor, a.clips);
    if (!r.ok) return bad(r.error);
    const ids = r.value.map((e) => e.item.id);
    await c.backend.apply([{ type: 'item.remove', itemIds: ids, ripple: a.ripple }], 'Delete clips');
    const after = await c.backend.snapshot();
    const left = c.snap.doc.items.length - ids.length;
    const f = [...V.itemsGone(after.doc, ids), ...(after.doc.items.length === left ? [] : [{ level: 'fail' as const, message: `expected ${left} items to remain, found ${after.doc.items.length}` }])];
    return done(`deleted ${r.value.map((e) => `${e.handle} "${e.name}"`).join(', ')}; ${lengthLine(c.snap, after)}`, f);
  },
});

export const moveClip = tool({
  name: 'moveClip',
  description:
    'Move a clip. Either before/after another clip on the same track (the clips then play back to back, no gaps) or to a timeline time with toSec.',
  input: z.object({ clip: clipArg, before: clipArg.optional(), after: clipArg.optional(), toSec: time.optional() }),
  mutates: true,
  async run(a, c) {
    const r = resolveClip(c.entries, c.snap.editor, a.clip);
    if (!r.ok) return bad(r.error);
    const anchorRef = a.before ?? a.after;
    if (anchorRef) {
      const anchor = resolveClip(c.entries, c.snap.editor, anchorRef);
      if (!anchor.ok) return bad(anchor.error);
      if (anchor.value.item.trackId !== r.value.item.trackId) return bad(`${r.value.handle} and ${anchor.value.handle} are on different tracks.`);
      const trackItems = c.snap.doc.items.filter((i) => i.trackId === r.value.item.trackId && i.type !== 'caption');
      const order = reorderedItems(trackItems, r.value.item, anchor.value.item, a.before ? 'before' : 'after');
      await c.backend.apply(layoutInOrderOps(order), 'Move clip');
      const after = await c.backend.snapshot();
      const f = [...V.inOrder(after.doc, order.map((i) => i.id)), ...V.changedSomething(c.snap.doc, after.doc, 'the move')];
      const names = order.map((i) => c.entries.find((e) => e.item.id === i.id)?.name ?? '?');
      return done(`order on ${r.value.trackLabel} is now ${names.join(' > ')}`, f);
    }
    const to = sec(a.toSec);
    if (to === null) return bad('Give before, after, or toSec.');
    const frame = toFrames(to, c.fps);
    const mover = r.value.item;
    // dropping a clip onto others on a media track means "insert there": reorder and close up, never stack them
    const others = c.snap.doc.items.filter((i) => i.trackId === mover.trackId && i.id !== mover.id && i.type !== 'caption');
    const hit = others.find((i) => frame < i.startFrame + i.durationFrames && frame + mover.durationFrames > i.startFrame);
    if (hit && (mover.type === 'video' || mover.type === 'audio')) {
      const where = frame <= hit.startFrame + hit.durationFrames / 2 ? 'before' : 'after';
      const order = reorderedItems([...others, mover], mover, hit, where);
      await c.backend.apply(layoutInOrderOps(order), 'Move clip');
      const after = await c.backend.snapshot();
      const names = order.map((i) => c.entries.find((e) => e.item.id === i.id)?.name ?? '?');
      return done(`order on ${r.value.trackLabel} is now ${names.join(' > ')}`, [...V.inOrder(after.doc, order.map((i) => i.id)), ...V.changedSomething(c.snap.doc, after.doc, 'the move')]);
    }
    await c.backend.apply([{ type: 'item.move', itemId: mover.id, startFrame: frame }], 'Move clip');
    const after = await c.backend.snapshot();
    return done(`moved ${r.value.handle} "${r.value.name}" to ${fmtTime(to)}`, [...V.itemRange(after.doc, mover.id, frame, mover.durationFrames), ...V.newOverlaps(c.snap.doc, after.doc, ['video', 'audio'])]);
  },
});

export const setClipProps = tool({
  name: 'setClipProps',
  description: 'Change a clip: volume (1 = normal, 0 = silent), speed (2 = twice as fast; the clip keeps the same content), muted, opacity (0-1) or scale (1 = normal).',
  input: z.object({
    clip: clipArg,
    volume: z.number().min(0).max(4).optional(),
    speed: z.number().min(0.1).max(16).optional(),
    muted: z.boolean().optional(),
    opacity: z.number().min(0).max(1).optional(),
    scale: z.number().min(0.05).max(10).optional(),
  }),
  mutates: true,
  async run(a, c) {
    const r = resolveClip(c.entries, c.snap.editor, a.clip);
    if (!r.ok) return bad(r.error);
    const item = r.value.item;
    const patch: Record<string, unknown> = {};
    const said: string[] = [];
    if (a.volume !== undefined) (patch['volume'] = a.volume), said.push(`volume ${a.volume}`);
    if (a.muted !== undefined) (patch['muted'] = a.muted), said.push(a.muted ? 'muted' : 'unmuted');
    if (a.speed !== undefined) {
      patch['speed'] = a.speed;
      patch['durationFrames'] = Math.max(1, Math.round((item.durationFrames * item.speed) / a.speed));
      said.push(`speed ${a.speed}x`);
    }
    const transform: Record<string, number> = {};
    if (a.opacity !== undefined) (transform['opacity'] = a.opacity), said.push(`opacity ${a.opacity}`);
    if (a.scale !== undefined) (transform['scale'] = a.scale), said.push(`scale ${a.scale}`);
    if (Object.keys(transform).length > 0) patch['transform'] = transform;
    if (Object.keys(patch).length === 0) return bad('Nothing to change: pass volume, speed, muted, opacity or scale.');
    const ops: Op[] = [{ type: 'item.update', itemId: item.id, patch: patch as never }];
    const newDuration = patch['durationFrames'] as number | undefined;
    if (newDuration !== undefined && newDuration !== item.durationFrames && (item.type === 'video' || item.type === 'audio')) {
      const delta = newDuration - item.durationFrames;
      const later = c.snap.doc.items.filter((i) => i.trackId === item.trackId && i.id !== item.id && i.startFrame >= item.startFrame + item.durationFrames).sort((x, y) => (delta > 0 ? y.startFrame - x.startFrame : x.startFrame - y.startFrame));
      const moves: Op[] = later.map((i) => ({ type: 'item.move', itemId: i.id, startFrame: i.startFrame + delta }));
      // growing: make room first; shrinking: close up after
      if (delta > 0) ops.unshift(...moves);
      else ops.push(...moves);
    }
    await c.backend.apply(ops, 'Edit clip');
    const after = await c.backend.snapshot();
    const now = after.doc.items.find((i) => i.id === item.id);
    const f: Finding[] = [...(newDuration !== undefined ? V.newOverlaps(c.snap.doc, after.doc, ['video', 'audio']) : [])];
    if (!now) f.push({ level: 'fail', message: 'the clip disappeared' });
    else {
      if (a.volume !== undefined && Math.abs(now.volume - a.volume) > 0.001) f.push({ level: 'fail', message: `volume is ${now.volume}` });
      if (a.speed !== undefined && Math.abs(now.speed - a.speed) > 0.001) f.push({ level: 'fail', message: `speed is ${now.speed}` });
    }
    return done(`${r.value.handle} "${r.value.name}": ${said.join(', ')}`, f);
  },
});

export const removeSection = tool({
  name: 'removeSection',
  description: 'Cut a stretch OUT of the video (remove / delete / take out everything between two times; seconds from the start of the video) on every clip it touches; everything after slides left to close the gap. The rest of the video stays.',
  input: z.object({ fromSec: time, toSec: time }),
  mutates: true,
  async run(a, c) {
    const from = sec(a.fromSec);
    const to = sec(a.toSec);
    if (from === null || to === null || to <= from) return bad('fromSec and toSec must be seconds with toSec after fromSec.');
    const f0 = toFrames(from, c.fps);
    const f1 = toFrames(to, c.fps);
    const items = c.snap.doc.items.filter((i) => (i.type === 'video' || i.type === 'audio') && i.startFrame < f1 && i.startFrame + i.durationFrames > f0).sort((x, y) => y.startFrame - x.startFrame);
    if (items.length === 0) return bad(`Nothing is on the timeline between ${fmtTime(from)} and ${fmtTime(to)}.`);
    const ops: Op[] = [];
    for (const i of items) ops.push(...cutSpansOps(i, [{ startFrame: Math.max(f0, i.startFrame), endFrame: Math.min(f1, i.startFrame + i.durationFrames) }]));
    await c.backend.apply(ops, 'Remove section');
    const after = await c.backend.snapshot();
    return done(`removed ${fmtTime(from)}–${fmtTime(to)}; ${lengthLine(c.snap, after)}`, V.changedSomething(c.snap.doc, after.doc, 'the cut'));
  },
});

// ---------------------------------------------------------------- speech cuts

export const removePauses = tool({
  name: 'removePauses',
  description: 'Cut the pauses (silences) out of the speech on the timeline. Pauses longer than minPauseSec go; a short handle stays so words are not clipped. Default 0.5 s.',
  input: z.object({ minPauseSec: z.number().min(0.1).max(5).default(0.5), clips: z.array(clipArg).optional().describe('Default: every clip with speech') }),
  mutates: true,
  async run(a, c) {
    const pick = speechClips(c, a.clips);
    if ('error' in pick) return bad(pick.error);
    let res: { cutSeconds?: number } | undefined;
    try {
      res = (await c.backend.call('removeSilences', { thresholdSec: a.minPauseSec, itemIds: pick.items.map((i) => i.id) })) as { cutSeconds?: number };
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      if (/No silences over the threshold/i.test(msg)) return done(`no pauses longer than ${fmtDur(a.minPauseSec)} found`, [], false);
      return bad(msg);
    }
    const after = await c.backend.snapshot();
    const assetIds = [...new Set(pick.items.map((i) => i.assetId!))];
    const silences: Record<string, { startMs: number; endMs: number }[] | null> = {};
    for (const id of assetIds) silences[id] = await c.backend.silences(id);
    const f = [...V.pausesGone(after.doc, silences, a.minPauseSec), ...V.changedSomething(c.snap.doc, after.doc, 'removing pauses')];
    return done(`cut ${fmtDur(res?.cutSeconds ?? 0)} of pauses longer than ${fmtDur(a.minPauseSec)}; ${lengthLine(c.snap, after)}`, f);
  },
});

export const removeFillers = tool({
  name: 'removeFillers',
  description: `Cut filler words out of the speech, found in the transcript. Default list: ${DEFAULT_FILLERS.join(', ')}. Pass words to use your own list.`,
  input: z.object({ words: z.array(z.string()).optional(), clips: z.array(clipArg).optional() }),
  mutates: true,
  async run(a, c) {
    const pick = speechClips(c, a.clips);
    if ('error' in pick) return bad(pick.error);
    const ops: Op[] = [];
    const byAsset: Record<string, [number, number][]> = {};
    let total = 0;
    for (const assetId of [...new Set(pick.items.map((i) => i.assetId!))]) {
      const words = c.snap.transcripts.find((t) => t.assetId === assetId)?.words ?? [];
      const ranges = fillerRanges(words, a.words && a.words.length > 0 ? a.words : DEFAULT_FILLERS);
      if (ranges.length === 0) continue;
      byAsset[assetId] = ranges;
      total += ranges.length;
      ops.push(...cutSourceRangesOps(c.snap, assetId, ranges, pick.items.map((i) => i.id)).ops);
    }
    if (total === 0) return done('no filler words found in the transcript', [], false);
    await c.backend.apply(ops, `Remove ${total} filler words`);
    const after = await c.backend.snapshot();
    const f = Object.entries(byAsset).flatMap(([assetId, ranges]) => V.fillersGone(after.doc, assetId, ranges));
    return done(`cut ${total} filler word${total === 1 ? '' : 's'}; ${lengthLine(c.snap, after)}`, f);
  },
});

export const removeRetakes = tool({
  name: 'removeRetakes',
  description: 'Remove abandoned takes: when the speaker starts a sentence, stops, and says it again, the earlier attempt is cut and the last one stays.',
  input: z.object({ clips: z.array(clipArg).optional() }),
  mutates: true,
  async run(a, c) {
    const pick = speechClips(c, a.clips);
    if ('error' in pick) return bad(pick.error);
    const ops: Op[] = [];
    let takes = 0;
    for (const item of [...pick.items].sort((x, y) => y.startFrame - x.startFrame)) {
      const r = retakeOps(c.snap, item);
      ops.push(...r.ops);
      takes += r.takes;
    }
    if (takes === 0) return done('no repeated takes found', [], false);
    await c.backend.apply(ops, `Remove ${takes} retake${takes === 1 ? '' : 's'}`);
    const after = await c.backend.snapshot();
    return done(`removed ${takes} repeated take${takes === 1 ? '' : 's'} (kept the last attempt); ${lengthLine(c.snap, after)}`, V.changedSomething(c.snap.doc, after.doc, 'removing retakes'));
  },
});

// ---------------------------------------------------------------- captions, titles

export const addCaptions = tool({
  name: 'addCaptions',
  description: 'Add captions (from the transcript) over the speech. Replaces captions already there. preset: karaoke (default, active word highlighted), bold, serif.',
  input: z.object({
    preset: z.enum(['karaoke', 'bold', 'serif']).default('karaoke'),
    wordsPerCard: z.number().int().min(1).max(12).default(4),
    clips: z.array(clipArg).optional(),
    color: z.string().max(20).optional().describe('Text colour when the user asks for one, e.g. "yellow" or "#ffff00"'),
    position: z.enum(['top', 'middle', 'bottom']).optional(),
    sizeFactor: z.number().min(0.3).max(4).optional().describe('1.3 = 30% bigger'),
  }),
  mutates: true,
  async run(a, c) {
    const pick = speechClips(c, a.clips);
    if ('error' in pick) return bad(pick.error);
    const old = c.snap.doc.items.filter((i) => i.type === 'caption').map((i) => i.id);
    if (old.length > 0) await c.backend.apply([{ type: 'item.remove', itemIds: old, ripple: false }], 'Replace captions');
    const res = (await c.backend.call('addCaptions', { itemIds: pick.items.map((i) => i.id), preset: a.preset, wordsPerCard: a.wordsPerCard })) as { captionCards?: number };
    let after = await c.backend.snapshot();
    const placementY = a.position === 'top' ? 0.12 : a.position === 'middle' ? 0.5 : a.position === 'bottom' ? 0.82 : undefined;
    let styled = '';
    if (a.color || placementY !== undefined || a.sizeFactor !== undefined) {
      const fresh = after.doc.items.filter((i) => i.type === 'caption');
      await c.backend.apply(captionStyleOps(fresh, { sizeFactor: a.sizeFactor, color: a.color, placementY }), 'Style captions');
      after = await c.backend.snapshot();
      styled = [a.color ? `colour ${a.color}` : '', a.position ? `at the ${a.position}` : '', a.sizeFactor ? `${a.sizeFactor}x size` : ''].filter(Boolean).join(', ');
    }
    return done(`added ${res.captionCards ?? '?'} caption cards (${a.preset}${styled ? `, ${styled}` : ''}) from ${fmtTime(0)} to ${fmtTime(V.docLengthFrames(after.doc) / c.fps)}${old.length ? `, replacing ${old.length} old ones` : ''}`, V.captionCoverage(after.doc));
  },
});

export const styleCaptions = tool({
  name: 'styleCaptions',
  description: 'Restyle ALL existing captions: sizeFactor (1.3 = 30% bigger, 0.8 = smaller) or an exact fontSize, color (e.g. "#ffffff"), position (top, middle, bottom).',
  input: z.object({ sizeFactor: z.number().min(0.3).max(4).optional(), fontSize: z.number().min(8).max(400).optional(), color: z.string().max(20).optional(), position: z.enum(['top', 'middle', 'bottom']).optional() }),
  mutates: true,
  async run(a, c) {
    const caps = c.snap.doc.items.filter((i) => i.type === 'caption');
    if (caps.length === 0) return bad('There are no captions yet; add them first.');
    const placementY = a.position === 'top' ? 0.12 : a.position === 'middle' ? 0.5 : a.position === 'bottom' ? 0.82 : undefined;
    const factor = a.sizeFactor === undefined && a.fontSize === undefined && !a.color && placementY === undefined ? 1.3 : a.sizeFactor;
    const ops = captionStyleOps(caps, { sizeFactor: factor, fontSize: a.fontSize, color: a.color, placementY });
    await c.backend.apply(ops, 'Restyle captions');
    const after = await c.backend.snapshot();
    const was = V.captionFontSize(c.snap.doc);
    const now = V.captionFontSize(after.doc);
    const f: Finding[] = [];
    if (after.doc.items.filter((i) => i.type === 'caption').length !== caps.length) f.push({ level: 'fail', message: 'caption cards were lost' });
    if ((factor !== undefined || a.fontSize !== undefined) && was === now) f.push({ level: 'fail', message: 'the caption size did not change' });
    return done(`restyled ${caps.length} caption cards${was !== now ? `: font size ${was} -> ${now}` : ''}`, f);
  },
});

export const addTitle = tool({
  name: 'addTitle',
  description: 'Add a title / text card on the text track. Defaults: starts at 0 s, lasts 3 s, centered.',
  input: z.object({ text: z.string().min(1).max(200), atSec: time.default(0), durationSec: z.number().min(0.2).max(600).default(3) }),
  mutates: true,
  async run(a, c) {
    const at = sec(a.atSec) ?? 0;
    const startFrame = toFrames(at, c.fps);
    const durationFrames = Math.max(1, toFrames(a.durationSec, c.fps));
    const same = (i: Item) => i.type === 'text' && i.startFrame === startFrame && i.durationFrames === durationFrames && String((i.props as { text?: string }).text ?? '').trim() === a.text.trim();
    if (c.snap.doc.items.some(same)) return done(`the title "${a.text}" is already at ${fmtTime(at)} for ${fmtDur(a.durationSec)}; nothing to add`, [], false);
    const style = { fontFamily: 'Geist', fontSize: Math.round(Math.min(c.snap.doc.project.width, c.snap.doc.project.height) / 15), fontWeight: 800, color: '#ffffff', strokeColor: '#000000', strokeWidth: 4, align: 'center', uppercase: false };
    await c.backend.call('addText', { text: a.text, startFrame, durationFrames, style });
    const after = await c.backend.snapshot();
    return done(`added the title "${a.text}" at ${fmtTime(at)} for ${fmtDur(a.durationSec)}`, V.titleExists(after.doc, a.text, startFrame, durationFrames));
  },
});

// ---------------------------------------------------------------- media, music, beats

export const addMedia = tool({
  name: 'addMedia',
  description: 'Put an imported asset on the timeline. atSec = where on the timeline (default: after the last clip); fromSec/toSec = which part of the asset (default: all).',
  input: z.object({ asset: z.string().describe('Asset file name'), atSec: time.optional(), fromSec: time.optional(), toSec: time.optional() }),
  mutates: true,
  async run(a, c) {
    const r = resolveAsset(c.snap.assets, a.asset);
    if (!r.ok) return bad(r.error);
    const asset = r.value;
    const from = a.fromSec === undefined ? 0 : sec(a.fromSec) ?? 0;
    const to = a.toSec === undefined ? undefined : sec(a.toSec) ?? undefined;
    const args: Record<string, unknown> = { assetId: asset.id };
    if (a.atSec !== undefined) args['startFrame'] = toFrames(sec(a.atSec) ?? 0, c.fps);
    if (from > 0) args['sourceInFrame'] = toFrames(from, c.fps);
    if (to !== undefined) args['durationFrames'] = Math.max(1, toFrames(to - from, c.fps));
    await c.backend.call(asset.kind === 'audio' ? 'addAudio' : 'addClip', asset.kind === 'audio' ? { ...args, startFrame: args['startFrame'] ?? 0 } : args);
    const after = await c.backend.snapshot();
    const f = after.doc.items.length === c.snap.doc.items.length + 1 ? [] : [{ level: 'fail' as const, message: 'the clip was not added' }];
    return done(`added "${asset.originalName}"; ${lengthLine(c.snap, after)}`, f);
  },
});

export const addMusic = tool({
  name: 'addMusic',
  description:
    'Put a music track under the whole video, quiet, and (by default) duck it so it dips while someone talks. volume 0-1 (default 0.25). Replaces the same track if it is already there.',
  input: z.object({ asset: z.string().describe('The music file name'), volume: z.number().min(0).max(1).default(0.25), duck: z.boolean().default(true) }),
  mutates: true,
  async run(a, c) {
    const r = resolveAsset(c.snap.assets, a.asset, 'audio');
    if (!r.ok) return bad(r.error);
    const asset = r.value;
    const lengthFrames = V.docLengthFrames(c.snap.doc);
    if (lengthFrames === 0) return bad('The timeline is empty; add footage before adding music.');
    const existing = c.snap.doc.items.filter((i) => i.type === 'audio' && i.assetId === asset.id).map((i) => i.id);
    if (existing.length > 0) await c.backend.apply([{ type: 'item.remove', itemIds: existing, ripple: false }], 'Replace music');
    const assetFrames = Math.round((asset.durationMs / 1000) * c.fps);
    const res = (await c.backend.call('addAudio', { assetId: asset.id, startFrame: 0, durationFrames: Math.min(assetFrames, lengthFrames), volume: a.volume })) as { items?: Item[] };
    const placed = (res.items ?? []).find((i) => i.assetId === asset.id) ?? (await c.backend.snapshot()).doc.items.find((i) => i.assetId === asset.id && i.type === 'audio');
    if (!placed) return bad('The music was not added.');
    let ducked = '';
    if (a.duck) {
      try {
        const d = (await c.backend.call('duckMusic', { musicItemId: placed.id })) as { duckedSpans?: number };
        ducked = `, dips under ${d.duckedSpans ?? 0} stretches of speech`;
      } catch (err) {
        ducked = `, not ducked (${err instanceof Error ? err.message.split('(')[0]!.trim() : 'no speech found'})`;
      }
    }
    const after = await c.backend.snapshot();
    const f = V.musicPlaced(after.doc, placed.id, lengthFrames, a.volume);
    if (a.duck && ducked.startsWith(', dips') && !V.hasVolumeEnvelope(after.doc, placed.id)) f.push({ level: 'fail', message: 'the ducking envelope was not written' });
    return done(`added "${asset.originalName}" under the video at volume ${a.volume}${ducked}`, f);
  },
});

export const cutToBeat = tool({
  name: 'cutToBeat',
  description: 'Re-cut the video so every cut lands on the beat of the music already on the timeline. Uses the imported video clips in order.',
  input: z.object({ music: z.string().optional().describe('Music clip handle or file name; default: the music on the timeline') }),
  mutates: true,
  async run(a, c) {
    const audio = c.entries.filter((e) => e.item.type === 'audio');
    const pick = a.music ? resolveClip(audio.length ? audio : c.entries, c.snap.editor, a.music) : audio.length === 1 ? ({ ok: true, value: audio[0]! } as const) : null;
    if (!pick) return bad(audio.length === 0 ? 'No music is on the timeline; add it first (addMusic).' : `Several audio clips: ${audio.map((e) => `${e.handle} "${e.name}"`).join(', ')}; say which one.`);
    if (!pick.ok) return bad(pick.error);
    const res = (await c.backend.call('beatSync', { musicItemId: pick.value.item.id, replaceExisting: true })) as { bpm?: number; cuts?: number };
    const after = await c.backend.snapshot();
    const vids = after.doc.items.filter((i) => i.type === 'video');
    const f = [...V.newOverlaps(c.snap.doc, after.doc), ...(vids.length > 1 ? [] : [{ level: 'fail' as const, message: 'fewer than two clips after cutting to the beat' }])];
    return done(`cut ${vids.length} clips to the beat of "${pick.value.name}" (${res.bpm ?? '?'} BPM)`, f);
  },
});

// ---------------------------------------------------------------- project, export, history

export const setCanvas = tool({
  name: 'setCanvas',
  description: 'Change the canvas shape for a platform: 9:16 (Reels, TikTok, Shorts), 16:9 (YouTube), 1:1, 4:5, 4:3, 21:9; and/or the frame rate.',
  input: z.object({ aspect: z.enum(['16:9', '9:16', '1:1', '4:5', '4:3', '21:9']).optional(), fps: z.number().int().min(1).max(120).optional() }),
  mutates: true,
  async run(a, c) {
    if (!a.aspect && !a.fps) return bad('Pass aspect and/or fps.');
    const res = (await c.backend.call('setProjectSettings', { aspect: a.aspect, fps: a.fps })) as { applied?: boolean; width?: number; height?: number; fps?: number; note?: string };
    const after = await c.backend.snapshot();
    const f = a.aspect ? V.canvasIs(after.doc, a.aspect) : [];
    if (res.applied === false) return done(`the project is already ${after.doc.project.width}x${after.doc.project.height} at ${after.doc.project.fps} fps`, [], false);
    return done(`canvas is now ${after.doc.project.width}x${after.doc.project.height} at ${after.doc.project.fps} fps`, f);
  },
});

export const exportVideo = tool({
  name: 'exportVideo',
  description: 'Export the finished video. preset: "720p", "1080p", "1440p" or a platform preset ("TikTok / Reels / Shorts", "YouTube 1080p", "Square 1080"). Default 1080p.',
  input: z.object({ preset: z.string().default('1080p') }),
  mutates: true, // an action (it does not edit the timeline)
  async run(a, c) {
    const res = (await c.backend.call('exportVideo', { preset: a.preset })) as { exportId?: string; status?: string };
    if (!res.exportId) return bad('The export did not start.');
    return done(`export started (${a.preset}); it renders in the background and the file appears in the export list`, [], false);
  },
});

export const undo = tool({
  name: 'undo',
  description: 'Undo the last N changes (default 1). One step = one whole job (e.g. everything done for one request).',
  input: z.object({ steps: z.number().int().min(1).max(20).default(1) }),
  mutates: true,
  async run(a, c) {
    c.backend.endGroup();
    const res = (await c.backend.call('undo', { steps: a.steps })) as { steps: number; labels: (string | null)[] };
    if (res.steps === 0) return done('there was nothing to undo', [], false);
    const after = await c.backend.snapshot();
    const f = V.changedSomething(c.snap.doc, after.doc, 'the undo');
    return done(`undid ${res.steps} change${res.steps === 1 ? '' : 's'}${res.labels.filter(Boolean).length ? ` (${res.labels.filter(Boolean).join(', ')})` : ''}`, f);
  },
});

export const redo = tool({
  name: 'redo',
  description: 'Redo the last N undone changes (default 1).',
  input: z.object({ steps: z.number().int().min(1).max(20).default(1) }),
  mutates: true,
  async run(a, c) {
    c.backend.endGroup();
    const res = (await c.backend.call('redo', { steps: a.steps })) as { steps: number };
    if (res.steps === 0) return done('there was nothing to redo', [], false);
    return done(`redid ${res.steps} change${res.steps === 1 ? '' : 's'}`, []);
  },
});

// ---------------------------------------------------------------- judgment-step tools

const rangeSchema = z.object({ asset: z.string().describe('Asset file name'), fromSec: time, toSec: time });

export const assembleSequence = tool({
  name: 'assembleSequence',
  description: 'Build the video from pieces of an asset: keep these source ranges (seconds of the original file), in this order, back to back from the start. Replaces that asset\'s clips on the main track.',
  input: z.object({ ranges: z.array(rangeSchema).min(1).max(40) }),
  mutates: true,
  async run(a, c) {
    const ranges = [];
    for (const r of a.ranges) {
      const asset = resolveAsset(c.snap.assets, r.asset);
      if (!asset.ok) return bad(asset.error);
      const from = sec(r.fromSec);
      const to = sec(r.toSec);
      if (from === null || to === null || to <= from) return bad(`Bad range ${String(r.fromSec)}–${String(r.toSec)}.`);
      ranges.push({ assetId: asset.value.id, startSec: from, endSec: to });
    }
    const { ops, totalFrames, itemIds } = assembleOps(c.snap, ranges);
    await c.backend.apply(ops, 'Assemble sequence');
    const after = await c.backend.snapshot();
    const f = V.inOrder(after.doc, itemIds, 0.1);
    return done(`built a ${fmtDur(totalFrames / c.fps)} sequence from ${ranges.length} range${ranges.length === 1 ? '' : 's'}; ${lengthLine(c.snap, after)}`, f);
  },
});

export const searchTranscript = tool({
  name: 'searchTranscript',
  description: 'Find where a word or phrase is said. Returns the asset and the time in seconds.',
  input: z.object({ phrase: z.string().min(1), asset: z.string().optional() }),
  mutates: false,
  async run(a, c) {
    const needle = a.phrase.toLowerCase().replace(/[^\p{L}\p{N}' ]/gu, '').trim();
    const hits: string[] = [];
    for (const t of c.snap.transcripts) {
      const asset = c.snap.assets.find((x) => x.id === t.assetId);
      if (!asset || (a.asset && !resolveAsset([asset], a.asset).ok)) continue;
      for (const s of sentencesOf(t.words)) {
        if (s.text.toLowerCase().replace(/[^\p{L}\p{N}' ]/gu, '').includes(needle)) hits.push(`"${asset.originalName}" ${fmtTime(s.startSec)}–${fmtTime(s.endSec)}: ${s.text}`);
      }
    }
    return done(hits.length ? hits.slice(0, 12).join('\n') : `"${a.phrase}" is not in any transcript`, [], false);
  },
});

export const readTranscript = tool({
  name: 'readTranscript',
  description: 'Read what is said in an asset as numbered sentences with times in seconds, optionally only between fromSec and toSec.',
  input: z.object({ asset: z.string(), fromSec: time.optional(), toSec: time.optional() }),
  mutates: false,
  async run(a, c) {
    const r = resolveAsset(c.snap.assets, a.asset);
    if (!r.ok) return bad(r.error);
    const t = c.snap.transcripts.find((x) => x.assetId === r.value.id);
    if (!t || t.words.length === 0) return done(`"${r.value.originalName}" has no transcript`, [], false);
    const from = a.fromSec === undefined ? 0 : sec(a.fromSec) ?? 0;
    const to = a.toSec === undefined ? Infinity : sec(a.toSec) ?? Infinity;
    const lines = sentencesOf(t.words)
      .filter((s) => s.endSec >= from && s.startSec <= to)
      .map((s) => `[${s.index}] ${fmtTime(s.startSec)}–${fmtTime(s.endSec)} ${s.text}`);
    return done(lines.join('\n'), [], false);
  },
});

export const FACADE: Record<string, FacadeTool> = Object.fromEntries(
  [
    trimClip,
    splitClip,
    deleteClips,
    moveClip,
    setClipProps,
    removeSection,
    removePauses,
    removeFillers,
    removeRetakes,
    addCaptions,
    styleCaptions,
    addTitle,
    addMedia,
    addMusic,
    cutToBeat,
    setCanvas,
    exportVideo,
    undo,
    redo,
    assembleSequence,
    searchTranscript,
    readTranscript,
  ].map((t) => [t.name, t]),
);

/** Sentences of an asset's transcript with end times pulled in to the audio (used by the judgment steps). */
export async function snappedSentences(backend: Backend, snap: Snapshot, assetId: string) {
  const words = snap.transcripts.find((t) => t.assetId === assetId)?.words ?? [];
  return snapToSpeech(sentencesOf(words), await backend.silences(assetId));
}

void mergeSpans;
