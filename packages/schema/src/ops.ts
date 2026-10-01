import { z } from 'zod';
import { frameSchema } from './time.ts';
import { itemPatchSchema, itemSchemas, itemPropsSchemas, timeRemapPointSchema } from './item.ts';
import { effectSchema, maskSchema } from './transform.ts';
import { markerSchema, trackKindSchema, styleConfigSchema } from './project.ts';
import { Keyframe, keyframeSchema } from './time.ts';

/**
 * item.add accepts items WITHOUT type-specific props (tools omit them); the apply
 * engine fills defaults. Every other field stays required.
 */
const itemAddLoose = z.discriminatedUnion('type', [
  itemSchemas.video.extend({ props: itemPropsSchemas.video.optional() }),
  itemSchemas.audio.extend({ props: itemPropsSchemas.audio.optional() }),
  itemSchemas.image.extend({ props: itemPropsSchemas.image.optional() }),
  itemSchemas.text.extend({ props: itemPropsSchemas.text.optional() }),
  itemSchemas.caption.extend({ props: itemPropsSchemas.caption.optional() }),
  itemSchemas.shape.extend({ props: itemPropsSchemas.shape.optional() }),
  itemSchemas.motionGraphic.extend({ props: itemPropsSchemas.motionGraphic.optional() }),
]);

/**
 * Ops are the only way timeline state changes. They are validated with zod, applied by
 * editor-core, appended to the per-project op log in SQLite, and drive undo/redo via
 * inverses computed at apply time. Ops never allocate ids — every id an op creates is
 * carried in the op itself so replay is deterministic.
 */
const opSchemaBase = z.discriminatedUnion('type', [
  z.object({ type: z.literal('project.rename'), name: z.string().min(1) }),
  z.object({ type: z.literal('project.setCanvas'), width: frameSchema.positive(), height: frameSchema.positive() }),
  z.object({ type: z.literal('project.setStyleConfig'), styleConfig: styleConfigSchema }),
  z.object({
    type: z.literal('project.setReference'),
    assetId: z.string().nullable().describe('Reference video for style matching; never used in output'),
  }),

  z.object({
    type: z.literal('track.add'),
    trackId: z.string(),
    kind: trackKindSchema,
    name: z.string(),
    locked: z.boolean().default(false),
    muted: z.boolean().default(false),
    hidden: z.boolean().default(false),
    /** insert position (0 = top); default: appended at the bottom */
    index: z.number().int().nonnegative().optional(),
  }),
  z.object({ type: z.literal('track.remove'), trackId: z.string() }),
  z.object({
    type: z.literal('track.update'),
    trackId: z.string(),
    patch: z.object({
      name: z.string().optional(),
      locked: z.boolean().optional(),
      muted: z.boolean().optional(),
      hidden: z.boolean().optional(),
    }),
  }),
  z.object({ type: z.literal('track.reorder'), trackIds: z.array(z.string()) }),

  z.object({ type: z.literal('item.add'), item: itemAddLoose }),
  z.object({
    type: z.literal('item.remove'),
    itemIds: z.array(z.string()).min(1),
    ripple: z.boolean().default(false),
  }),
  z.object({ type: z.literal('item.update'), itemId: z.string(), patch: itemPatchSchema }),
  z.object({
    type: z.literal('item.move'),
    itemId: z.string(),
    trackId: z.string().optional(),
    startFrame: frameSchema.optional(),
  }),
  z.object({
    type: z.literal('item.trim'),
    itemId: z.string(),
    edge: z.enum(['in', 'out']),
    frame: frameSchema,
    ripple: z.boolean().default(false),
  }),
  z.object({
    type: z.literal('item.split'),
    itemId: z.string(),
    atFrame: frameSchema,
    newItemId: z.string(),
  }),
  z.object({
    type: z.literal('item.clone'),
    itemId: z.string(),
    newItemId: z.string(),
    trackId: z.string().optional(),
    startFrame: frameSchema.optional(),
  }),
  z.object({ type: z.literal('item.slip'), itemId: z.string(), sourceInFrame: frameSchema }),
  z.object({ type: z.literal('item.setSpeed'), itemId: z.string(), speed: z.number().min(0.1).max(16) }),
  z.object({
    type: z.literal('item.setTimeRemap'),
    itemId: z.string(),
    points: z.array(timeRemapPointSchema),
  }),
  z.object({
    type: z.literal('item.setKeyframes'),
    itemId: z.string(),
    property: z.string(),
    keyframes: z.array(keyframeSchema),
  }),

  z.object({ type: z.literal('effect.add'), itemId: z.string(), effect: effectSchema }),
  z.object({ type: z.literal('effect.remove'), itemId: z.string(), effectId: z.string() }),
  z.object({
    type: z.literal('effect.update'),
    itemId: z.string(),
    effectId: z.string(),
    patch: z.object({ type: z.string().optional(), params: z.record(z.string(), z.union([z.number(), z.string(), z.boolean()])).optional() }),
  }),

  z.object({ type: z.literal('mask.add'), itemId: z.string(), mask: maskSchema }),
  z.object({ type: z.literal('mask.remove'), itemId: z.string(), maskId: z.string() }),

  z.object({ type: z.literal('marker.add'), marker: markerSchema }),
  z.object({ type: z.literal('marker.remove'), markerId: z.string() }),
  z.object({
    type: z.literal('marker.update'),
    markerId: z.string(),
    patch: z.object({ frame: frameSchema.optional(), label: z.string().optional(), color: z.string().optional() }),
  }),
]);

export type Op = z.infer<typeof opSchemaBase> | BatchOp;

export interface BatchOp {
  type: 'batch';
  /** Atomic: all ops apply or none. Nested batches are rejected. */
  ops: Op[];
}

export const opSchema: z.ZodType<Op> = z.union([opSchemaBase, z.lazy(() => batchSchema)]);

const batchSchema: z.ZodType<BatchOp> = z.object({
  type: z.literal('batch'),
  ops: z.array(opSchema).min(1),
});

/** One committed change: a single op or a batch, plus who made it. */
export interface OpEntry {
  seq: number;
  actor: import('./ids.ts').Actor;
  op: Op;
  createdAt: string;
}

export { itemPropsSchemas };
