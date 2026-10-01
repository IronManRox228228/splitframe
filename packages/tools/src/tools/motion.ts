import { z } from 'zod';
import { newId, type Item } from '@cutboard/schema';
import type { ToolDef } from '../registry.ts';

/**
 * Motion-graphics tools (main prompt §1.2 + §7): the agent writes a single function
 * expression using the allowlisted API (useCurrentFrame, interpolate, spring, Sequence,
 * AbsoluteFill, Box, Text, Img, Shape) that returns a scene tree per frame. Graphics are
 * edited ONLY through chat (never drag handles); code errors return the message + a
 * rendered frame for the auto-repair loop (max 3 retries).
 */

interface Snapshot {
  doc: {
    project: { fps: number; width: number; height: number };
    tracks: { id: string; kind: string; name: string; locked: boolean }[];
    items: Item[];
  };
}

function requireSnapshot(snap: unknown): Snapshot {
  const s = snap as Snapshot;
  if (!s || typeof s !== 'object' || !('doc' in s)) throw new Error('No project open.');
  return s;
}

const API_DOC = `Allowed API (a Remotion-style scene-tree API — return plain objects, no JSX/React):
- useCurrentFrame(): number — item-local frame for this render
- useVideoConfig(): { width, height, fps, durationInFrames }
- interpolate(input, [in0,in1], [out0,out1], { easing }) — map a value
- spring({ frame, fps, config: { damping, mass, stiffness } }) → 0..1 progress
- Sequence({ from, durationInFrames, children }) — time-shift children (children may be a function (localFrame) => node)
- AbsoluteFill(props), Box({ x, y, width, height, fill, radius, opacity, rotation }), Text({ text, x, y, fontSize, fontWeight, fontFamily, color, align, uppercase, strokeWidth, strokeColor }), Img({ src: 'asset:<assetId>', x, y, width, height }), Shape({ shape-ish ellipse: x, y, width, height, fill })
Nodes: { type: 'box'|'text'|'img'|'ellipse', x, y, width, height, ... }. x/y are canvas px, centered. Return the root node (or null before entrance).`;

export const createMotionGraphic: ToolDef = {
  name: 'createMotionGraphic',
  description:
    `Create an animated graphic (lower third, title card, animated list, counter) from code you write. ${API_DOC} IMPORTANT: never fetch data or touch the DOM — computation only. Time to transcript word timings (getTranscript) with Sequence/from. After creating, call previewMotionGraphic to SEE it and fix errors (you get the error message and a rendered frame on failure).`,
  input: z.object({
    name: z.string().min(1).max(60).describe('Human label, e.g. "Lower third — Dana"'),
    startFrame: z.number().int().nonnegative(),
    durationFrames: z.number().int().min(1),
    code: z.string().min(10).max(40000).describe('Single function expression: (props) => sceneNode'),
    inputProps: z.record(z.string(), z.unknown()).default({}).describe('Props passed to the component, e.g. { words: [...], title: "..." }'),
    trackId: z.string().optional(),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const check = ctx.validateMotion(input.code);
    if (!check.ok) throw new Error(`Motion code rejected: ${check.error}. Rewrite it using only the allowed API.`);
    const track =
      (input.trackId ? snapshot.doc.tracks.find((t) => t.id === input.trackId) : undefined) ??
      snapshot.doc.tracks.find((t) => t.kind === 'text');
    if (!track) throw new Error('No text track available.');
    const item = {
      id: newId('itm'),
      trackId: track.id,
      type: 'motionGraphic',
      startFrame: input.startFrame,
      durationFrames: input.durationFrames,
      transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
      props: { code: input.code, inputProps: input.inputProps },
      labels: { name: input.name },
    };
    await ctx.applyOps([{ type: 'item.add', item: item as never }], ctx.actor, `createMotionGraphic: ${input.name}`);
    return {
      applied: true,
      itemId: item.id,
      note: 'Call previewMotionGraphic(frame) to verify visually — especially the first and last frames.',
    };
  },
};

export const updateMotionGraphic: ToolDef = {
  name: 'updateMotionGraphic',
  description:
    'Patch a motion graphic: replace the code, update inputProps, or retime it. Graphics are edited ONLY through this tool (no drag handles). This stores a new version; undo restores the previous code.',
  input: z.object({
    itemId: z.string(),
    code: z.string().min(10).max(40000).optional(),
    inputProps: z.record(z.string(), z.unknown()).optional().describe('Whole-replace of inputProps'),
    startFrame: z.number().int().nonnegative().optional(),
    durationFrames: z.number().int().min(1).optional(),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const item = snapshot.doc.items.find((i) => i.id === input.itemId);
    if (!item || item.type !== 'motionGraphic') throw new Error(`Motion graphic item ${input.itemId} not found.`);
    const patch: Record<string, unknown> = {};
    if (input.code !== undefined) {
      const check = ctx.validateMotion(input.code);
      if (!check.ok) throw new Error(`Motion code rejected: ${check.error}. Rewrite it using only the allowed API.`);
      patch.props = { code: input.code, inputProps: input.inputProps ?? item.props.inputProps };
    } else if (input.inputProps !== undefined) {
      patch.props = { code: item.props.code, inputProps: input.inputProps };
    }
    if (input.startFrame !== undefined) patch.startFrame = input.startFrame;
    if (input.durationFrames !== undefined) patch.durationFrames = input.durationFrames;
    await ctx.applyOps([{ type: 'item.update', itemId: input.itemId, patch: patch as never }], ctx.actor, 'updateMotionGraphic');
    return { applied: true, itemId: input.itemId, note: 'Verify with previewMotionGraphic.' };
  },
};

export const previewMotionGraphic: ToolDef = {
  name: 'previewMotionGraphic',
  description:
    'Render a frame of the timeline (including motion graphics) and return it as an image. If a motion graphic errors, the image shows the error — read it, fix the code with updateMotionGraphic, and preview again (max 3 repair attempts).',
  input: z.object({
    frame: z.number().int().nonnegative().describe('Timeline frame to render'),
    width: z.number().int().positive().max(1920).optional(),
  }),
  mutates: false,
  async handler(input, ctx) {
    const png = await ctx.captureFrame(input.frame, input.width);
    return { format: 'image/png', frame: input.frame, base64: png.toString('base64') };
  },
};

export const MOTION_TOOLS: ToolDef[] = [createMotionGraphic, updateMotionGraphic, previewMotionGraphic];
