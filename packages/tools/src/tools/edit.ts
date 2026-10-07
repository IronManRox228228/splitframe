import { z } from 'zod';
import { newId, frameSchema, type Op } from '@cutboard/schema';
import { createItem, docDurationFrames, trackAllowsItem } from '@cutboard/editor-core';
import type { ToolDef } from '../registry.ts';

/**
 * Timeline edit tools (main prompt §6): thin, typed wrappers over the op system.
 * Mutating tools return the affected items' new state so agents can self-verify.
 * All ops go through ctx.applyOps — the main process validates and persists them.
 */

const textStyleInput = z.object({
  fontFamily: z.string().default('Geist'),
  fontSize: z.number().positive().default(72),
  fontWeight: z.number().int().min(100).max(1000).default(800),
  color: z.string().default('#ffffff'),
  strokeColor: z.string().optional(),
  strokeWidth: z.number().min(0).default(4),
  backgroundColor: z.string().optional(),
  align: z.enum(['left', 'center', 'right']).default('center'),
  uppercase: z.boolean().default(false),
});

function summarize(snapshot: unknown, itemIds: string[]): unknown {
  // callers pass ctx.getSnapshot(), i.e. { doc, assets, ... }; the items live on .doc
  const d = (snapshot as { doc?: { items?: { id: string }[] } } | undefined)?.doc;
  const items = (d?.items ?? []).filter((i) => itemIds.includes(i.id));
  return { applied: true, items, itemCount: d?.items?.length ?? 0 };
}

function requireSnapshot(snap: unknown): { doc: { tracks: { id: string; kind: string; name: string; locked: boolean }[]; items: { id: string; trackId: string; startFrame: number; durationFrames: number }[]; project: { fps: number }; markers: unknown[] }; assets: { id: string; kind: string; durationMs?: number }[] } {
  const s = snap as never;
  if (!s || typeof s !== 'object' || !('doc' in s)) {
    throw new Error('No project open. Open or create a project first.');
  }
  return s;
}

export const addClip: ToolDef = {
  name: 'addClip',
  description:
    'Add a video/image asset to the timeline. Defaults: appends after the last item on the first compatible video track, snapped to nearby edges. Returns the new item (verify with getTimeline).',
  input: z.object({
    assetId: z.string().describe('Asset id from listAssets'),
    trackId: z.string().optional().describe('Target track; defaults to the first compatible video track'),
    startFrame: frameSchema.optional().describe('Timeline position; default = end of that track'),
    durationFrames: frameSchema.optional().describe('Trim to this length; default = full asset'),
    sourceInFrame: frameSchema.optional().describe('Start partway into the asset'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snap = requireSnapshot(await ctx.getSnapshot());
    const asset = snap.assets.find((a) => a.id === input.assetId);
    if (!asset) throw new Error(`Asset ${input.assetId} not found. Call listAssets first.`);
    const fps = snap.doc.project.fps;
    const itemType = asset.kind === 'audio' ? 'audio' : asset.kind === 'image' ? 'image' : 'video';
    // the first unlocked track that can hold this kind of item (audio goes to an audio track)
    const track =
      (input.trackId ? snap.doc.tracks.find((t) => t.id === input.trackId) : undefined) ??
      snap.doc.tracks.find((t) => !t.locked && trackAllowsItem(t.kind as never, itemType));
    if (!track) throw new Error(`No unlocked track can hold a ${itemType} clip. Create one with the UI or use batchEdit after track.add.`);
    if (track.locked) throw new Error(`Track "${track.name}" is locked. Unlock it first.`);
    const start =
      input.startFrame ??
      snap.doc.items
        .filter((i) => i.trackId === track.id)
        .reduce((end, i) => Math.max(end, i.startFrame + i.durationFrames), 0);
    // default = the whole asset from sourceIn (stills get 5 seconds)
    const sourceIn = input.sourceInFrame ?? 0;
    const assetFrames = asset.durationMs ? Math.round((asset.durationMs / 1000) * fps) : Math.round(fps);
    const duration =
      input.durationFrames ?? (itemType === 'image' ? Math.round(fps * 5) : Math.max(1, assetFrames - sourceIn));
    const item = createItem(itemType, {
      id: newId('itm'),
      trackId: track.id,
      startFrame: Math.max(0, start),
      durationFrames: duration,
      assetId: asset.id,
      sourceInFrame: sourceIn,
      labels: { name: `clip` },
    } as never);
    const ops: Op[] = [{ type: 'item.add', item: item as never }];
    const { inverses } = await ctx.applyOps(ops, ctx.actor, 'addClip');
    const fresh = (await ctx.getSnapshot()) as { doc: { items: { id: string; durationFrames: number }[] } };
    const placed = fresh.doc.items.find((i) => i.id === item.id);
    return { applied: true, item: placed ?? item, undoable: inverses.length > 0 };
  },
};

export const addText: ToolDef = {
  name: 'addText',
  description:
    'Add a text item (title, callout, list) on the text track. Time it with startFrame/durationFrames — use the transcript word timings (getTranscript) to sync text to speech.',
  input: z.object({
    text: z.string().min(1).max(500),
    startFrame: frameSchema,
    durationFrames: frameSchema.min(1),
    trackId: z.string().optional(),
    style: textStyleInput.optional(),
    x: z.number().optional().describe('Offset from center in canvas px'),
    y: z.number().optional(),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snap = requireSnapshot(await ctx.getSnapshot());
    const track = (input.trackId ? snap.doc.tracks.find((t) => t.id === input.trackId) : undefined) ?? snap.doc.tracks.find((t) => t.kind === 'text' && !t.locked);
    if (!track) throw new Error('No unlocked text track available.');
    if (track.locked) throw new Error(`Track "${track.name}" is locked. Unlock it first.`);
    const item = createItem('text', {
      id: newId('itm'),
      trackId: track.id,
      startFrame: Math.max(0, input.startFrame),
      durationFrames: input.durationFrames,
      transform: { x: input.x ?? 0, y: input.y ?? 0 } as never,
      labels: { name: input.text.slice(0, 24) },
      props: { text: input.text, style: input.style ?? textStyleInput.parse({}) } as never,
    } as never);
    await ctx.applyOps([{ type: 'item.add', item: item as never }], ctx.actor, 'addText');
    return summarize(await ctx.getSnapshot(), [item.id]);
  },
};

export const updateItem: ToolDef = {
  name: 'updateItem',
  description:
    'Change one item: transform (x/y/scale/rotation/opacity), volume, speed, or type-specific props (replace the whole props object). Send only the fields you want to change.',
  input: z.object({
    itemId: z.string(),
    patch: z
      .object({
        startFrame: frameSchema.optional(),
        durationFrames: frameSchema.optional(),
        sourceInFrame: frameSchema.optional(),
        speed: z.number().min(0.1).max(16).optional(),
        volume: z.number().min(0).optional(),
        muted: z.boolean().optional(),
        transform: z
          .object({
            x: z.number().optional(),
            y: z.number().optional(),
            scale: z.number().positive().optional(),
            scaleX: z.number().positive().optional(),
            scaleY: z.number().positive().optional(),
            rotation: z.number().optional(),
            opacity: z.number().min(0).max(1).optional(),
          })
          .optional(),
        props: z.record(z.string(), z.unknown()).optional().describe('Whole-replace of the type-specific props'),
      })
      .describe('Fields to change; transform fields merge'),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps(
      [{ type: 'item.update', itemId: input.itemId, patch: input.patch as never }],
      ctx.actor,
      'updateItem',
    );
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const moveItem: ToolDef = {
  name: 'moveItem',
  description: 'Move an item to another position (and optionally another compatible track).',
  input: z.object({
    itemId: z.string(),
    trackId: z.string().optional(),
    startFrame: frameSchema.optional(),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps(
      [{ type: 'item.move', itemId: input.itemId, trackId: input.trackId, startFrame: input.startFrame }],
      ctx.actor,
      'moveItem',
    );
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const trimItem: ToolDef = {
  name: 'trimItem',
  description:
    'Trim one edge of an item to a timeline frame. edge "in" keeps the end fixed; edge "out" keeps the start fixed. Use captureFrame on nearby frames to find exact cut points.',
  input: z.object({
    itemId: z.string(),
    edge: z.enum(['in', 'out']),
    frame: frameSchema,
    ripple: z.boolean().default(false).describe('Ripple: downstream items slide by the trim amount'),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps(
      [{ type: 'item.trim', itemId: input.itemId, edge: input.edge, frame: input.frame, ripple: input.ripple }],
      ctx.actor,
      'trimItem',
    );
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const splitItem: ToolDef = {
  name: 'splitItem',
  description: 'Split an item at a timeline frame into two independent items. Returns both halves with their ids.',
  input: z.object({ itemId: z.string(), atFrame: frameSchema }),
  mutates: true,
  async handler(input, ctx) {
    const snap = requireSnapshot(await ctx.getSnapshot());
    const item = snap.doc.items.find((i) => i.id === input.itemId);
    if (!item) throw new Error(`Item ${input.itemId} not found. Call getTimeline.`);
    const newItemId = newId('itm');
    await ctx.applyOps(
      [{ type: 'item.split', itemId: input.itemId, atFrame: input.atFrame, newItemId }],
      ctx.actor,
      'splitItem',
    );
    return summarize(await ctx.getSnapshot(), [input.itemId, newItemId]);
  },
};

export const cloneItem: ToolDef = {
  name: 'cloneItem',
  description: 'Duplicate an item (placed right after the original) for repeatable patterns.',
  input: z.object({ itemId: z.string(), trackId: z.string().optional(), startFrame: frameSchema.optional() }),
  mutates: true,
  async handler(input, ctx) {
    const newItemId = newId('itm');
    await ctx.applyOps(
      [{ type: 'item.clone', itemId: input.itemId, newItemId, trackId: input.trackId, startFrame: input.startFrame }],
      ctx.actor,
      'cloneItem',
    );
    return summarize(await ctx.getSnapshot(), [newItemId]);
  },
};

export const deleteItems: ToolDef = {
  name: 'deleteItems',
  description: 'Remove one or more items. ripple=true closes the gap (recommended for removing pauses/retakes).',
  input: z.object({
    itemIds: z.array(z.string()).min(1),
    ripple: z.boolean().default(false),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps([{ type: 'item.remove', itemIds: input.itemIds, ripple: input.ripple }], ctx.actor, 'deleteItems');
    return { applied: true, removed: input.itemIds.length };
  },
};

export const slipItem: ToolDef = {
  name: 'slipItem',
  description: 'Shift which part of the source plays without moving the item in the timeline (slip). Media items only.',
  input: z.object({ itemId: z.string(), sourceInFrame: frameSchema }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps([{ type: 'item.slip', itemId: input.itemId, sourceInFrame: input.sourceInFrame }], ctx.actor, 'slipItem');
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const setTimeRemap: ToolDef = {
  name: 'setTimeRemap',
  description:
    'Speed-ramp an item with a piecewise-linear map of item-local frames → source frames (e.g. slow-mo 0.5x for the first 90 frames, then 2x). Replaces any existing remap; empty list clears it.',
  input: z.object({
    itemId: z.string(),
    points: z.array(z.object({ frame: frameSchema, sourceFrame: frameSchema })).min(0).max(50),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps([{ type: 'item.setTimeRemap', itemId: input.itemId, points: input.points }], ctx.actor, 'setTimeRemap');
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const setKeyframes: ToolDef = {
  name: 'setKeyframes',
  description:
    'Animate any numeric property: set the full keyframe list for one property path (transform.scale, transform.opacity, transform.x, transform.y, transform.rotation, volume). Keyframes = [{frame, value, easing}].',
  input: z.object({
    itemId: z.string(),
    property: z.string(),
    keyframes: z
      .array(z.object({ frame: frameSchema, value: z.number(), easing: z.enum(['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut']).default('linear') }))
      .max(200),
  }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps(
      [{ type: 'item.setKeyframes', itemId: input.itemId, property: input.property, keyframes: input.keyframes }],
      ctx.actor,
      'setKeyframes',
    );
    return summarize(await ctx.getSnapshot(), [input.itemId]);
  },
};

export const addMarker: ToolDef = {
  name: 'addMarker',
  description: 'Drop a labeled marker on the timeline ruler (e.g. "drop", "chorus starts").',
  input: z.object({ frame: frameSchema, label: z.string().min(1).max(80), color: z.string().optional() }),
  mutates: true,
  async handler(input, ctx) {
    const marker = { id: newId('mrk'), frame: Math.max(0, input.frame), label: input.label, color: input.color };
    await ctx.applyOps([{ type: 'marker.add', marker }], ctx.actor, 'addMarker');
    return { applied: true, marker };
  },
};

export const batchEdit: ToolDef = {
  name: 'batchEdit',
  description:
    'Apply several edit operations atomically (all-or-nothing). Prefer macro tools when available; use batchEdit for multi-item surgery like removing all silences in one commit.',
  input: z.object({
    label: z.string().max(80).optional(),
    ops: z.array(z.unknown()).min(1).max(100).describe('Ops in the same shape the editor uses internally (item.add, item.trim, …)'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const { opSchema } = await import('@cutboard/schema');
    const ops = (input.ops as unknown[]).map((o) => opSchema.parse(o));
    await ctx.applyOps(ops, ctx.actor, input.label ?? 'batchEdit');
    return { applied: true, count: ops.length };
  },
};

export const addAudio: ToolDef = {
  name: 'addAudio',
  description: 'Add an audio asset (music bed, SFX, voiceover) to the audio track at a position.',
  input: z.object({
    assetId: z.string(),
    startFrame: frameSchema.optional().describe('Default 0'),
    durationFrames: frameSchema.optional().describe('Trim length; default full asset'),
    volume: z.number().min(0).max(1).default(1),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snap = requireSnapshot(await ctx.getSnapshot());
    const asset = snap.assets.find((a) => a.id === input.assetId);
    if (!asset) throw new Error(`Asset ${input.assetId} not found.`);
    if (asset.kind !== 'audio') throw new Error(`Asset ${input.assetId} is ${asset.kind}, not audio. Use addClip for video.`);
    const track = snap.doc.tracks.find((t) => t.kind === 'audio' && !t.locked);
    if (!track) throw new Error('No unlocked audio track available.');
    const fps = snap.doc.project.fps;
    // default = the whole asset
    const duration = input.durationFrames ?? Math.max(1, asset.durationMs ? Math.round((asset.durationMs / 1000) * fps) : Math.round(fps * 10));
    const item = createItem('audio', {
      id: newId('itm'),
      trackId: track.id,
      startFrame: Math.max(0, input.startFrame ?? 0),
      durationFrames: duration,
      assetId: asset.id,
      volume: input.volume,
      labels: { name: 'audio' },
    } as never);
    await ctx.applyOps([{ type: 'item.add', item: item as never }], ctx.actor, 'addAudio');
    return summarize(await ctx.getSnapshot(), [item.id]);
  },
};

export const getTimelineDuration: ToolDef = {
  name: 'getTimelineDuration',
  description: 'Get the total timeline duration in frames and seconds — quick check for "is my rough cut under 45s?".',
  input: z.object({}),
  mutates: false,
  async handler(_input, ctx) {
    const snap = requireSnapshot(await ctx.getSnapshot());
    const frames = docDurationFrames(snap.doc as never);
    const fps = snap.doc.project.fps;
    return { frames, seconds: Number((frames / fps).toFixed(2)), fps };
  },
};

export const EDIT_TOOLS: ToolDef[] = [
  addClip,
  addAudio,
  addText,
  updateItem,
  moveItem,
  trimItem,
  splitItem,
  cloneItem,
  deleteItems,
  slipItem,
  setTimeRemap,
  setKeyframes,
  addMarker,
  batchEdit,
  getTimelineDuration,
];
