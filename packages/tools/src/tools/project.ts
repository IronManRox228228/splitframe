import { z } from 'zod';
import type { Item, Op } from '@cutboard/schema';
import type { ToolDef } from '../registry.ts';

/** Project-level tools: undo/redo through the shared history, canvas size and frame rate. */

type HistoryResult = { steps: number; labels: (string | null)[]; canUndo: boolean; canRedo: boolean };

const historyResult = (r: HistoryResult, verb: 'undo' | 'redo') => ({
  steps: r.steps,
  labels: r.labels,
  canUndo: r.canUndo,
  canRedo: r.canRedo,
  note: r.steps === 0 ? `Nothing to ${verb}.` : 'Verify with getTimeline.',
});

export const undo: ToolDef = {
  name: 'undo',
  description:
    'Undo the last N edits (default 1), newest first. One step = one tool call that changed the project (or one user edit). Returns the labels undone. Use for "undo that" / "put it back"; do not re-edit by hand.',
  input: z.object({ steps: z.number().int().min(1).max(50).default(1).describe('How many edits to undo') }),
  mutates: true,
  async handler(input, ctx) {
    return historyResult(await ctx.undo(input.steps), 'undo');
  },
};

export const redo: ToolDef = {
  name: 'redo',
  description: 'Redo the last N undone edits (default 1). Only works until a new edit is made after an undo.',
  input: z.object({ steps: z.number().int().min(1).max(50).default(1).describe('How many undone edits to redo') }),
  mutates: true,
  async handler(input, ctx) {
    return historyResult(await ctx.redo(input.steps), 'redo');
  },
};

const ASPECTS: Record<string, [number, number]> = {
  '16:9': [16, 9],
  '9:16': [9, 16],
  '1:1': [1, 1],
  '4:5': [4, 5],
  '5:4': [5, 4],
  '4:3': [4, 3],
  '3:4': [3, 4],
  '21:9': [21, 9],
};

const even = (n: number) => Math.max(2, Math.round(n / 2) * 2);

/** Canvas size for an aspect ratio whose short side is `shortSide` px. */
export function canvasForAspect(aspect: string, shortSide: number): { width: number; height: number } {
  const [aw, ah] = ASPECTS[aspect] ?? [16, 9];
  return aw >= ah
    ? { width: even((shortSide * aw) / ah), height: even(shortSide) }
    : { width: even(shortSide), height: even((shortSide * ah) / aw) };
}

/** Ops that move existing items so they keep their place relative to the canvas when it is resized. */
export function relayoutOps(items: Item[], from: { width: number; height: number }, to: { width: number; height: number }): Op[] {
  const rx = to.width / from.width;
  const ry = to.height / from.height;
  const ops: Op[] = [];
  for (const it of items) {
    const kf = it.keyframes ?? {};
    const patch: { transform?: { x: number; y: number }; keyframes?: Item['keyframes'] } = {};
    if (it.transform.x !== 0 || it.transform.y !== 0) {
      patch.transform = { x: Math.round(it.transform.x * rx), y: Math.round(it.transform.y * ry) };
    }
    if (kf['transform.x'] || kf['transform.y']) {
      patch.keyframes = { ...kf };
      if (kf['transform.x']) patch.keyframes['transform.x'] = kf['transform.x'].map((k) => ({ ...k, value: k.value * rx }));
      if (kf['transform.y']) patch.keyframes['transform.y'] = kf['transform.y'].map((k) => ({ ...k, value: k.value * ry }));
    }
    if (patch.transform || patch.keyframes) ops.push({ type: 'item.update', itemId: it.id, patch: patch as never });
  }
  return ops;
}

export const setProjectSettings: ToolDef = {
  name: 'setProjectSettings',
  description:
    'Change the project canvas and/or frame rate. Canvas: an aspect ("9:16" Reels/TikTok/Shorts, "16:9", "1:1", "4:5", "4:3", "21:9") with an optional shortSidePx (default 1080, so 9:16 = 1080x1920), or explicit width+height. Items keep their place (offsets scale with the canvas); an fps change keeps every cut at the same time in seconds. One undo reverts it.',
  input: z.object({
    aspect: z.enum(['16:9', '9:16', '1:1', '4:5', '5:4', '4:3', '3:4', '21:9']).optional(),
    shortSidePx: z.number().int().min(240).max(4320).default(1080).describe('Short side in pixels when using aspect'),
    width: z.number().int().min(64).max(7680).optional().describe('Explicit canvas width (with height)'),
    height: z.number().int().min(64).max(7680).optional().describe('Explicit canvas height (with width)'),
    fps: z.number().int().min(1).max(120).optional().describe('Frames per second, e.g. 24, 30, 60'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snap = (await ctx.getSnapshot()) as { doc: { project: { width: number; height: number; fps: number }; items: Item[] } };
    const p = snap.doc.project;
    let target: { width: number; height: number } | undefined;
    if (input.width !== undefined || input.height !== undefined) {
      if (input.width === undefined || input.height === undefined) throw new Error('Pass both width and height, or use aspect.');
      target = { width: even(input.width), height: even(input.height) };
    } else if (input.aspect) {
      target = canvasForAspect(input.aspect, input.shortSidePx);
    }
    if (!target && input.fps === undefined) throw new Error('Nothing to change: pass aspect, width+height and/or fps.');
    const ops: Op[] = [];
    const changed: string[] = [];
    if (target && (target.width !== p.width || target.height !== p.height)) {
      ops.push({ type: 'project.setCanvas', width: target.width, height: target.height }, ...relayoutOps(snap.doc.items, p, target));
      changed.push(`canvas ${p.width}x${p.height} -> ${target.width}x${target.height}`);
    }
    if (input.fps !== undefined && input.fps !== p.fps) {
      ops.push({ type: 'project.setFps', fps: input.fps });
      changed.push(`fps ${p.fps} -> ${input.fps}`);
    }
    if (ops.length === 0) return { applied: false, note: 'Project already has these settings.', width: p.width, height: p.height, fps: p.fps };
    await ctx.applyOps([{ type: 'batch', ops }], ctx.actor, 'setProjectSettings');
    return {
      applied: true,
      changed,
      width: target?.width ?? p.width,
      height: target?.height ?? p.height,
      fps: input.fps ?? p.fps,
      note: 'Verify with getTimeline / captureFrame.',
    };
  },
};

export const PROJECT_TOOLS: ToolDef[] = [undo, redo, setProjectSettings];
