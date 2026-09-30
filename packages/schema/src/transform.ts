import { z } from 'zod';
import { keyframeSchema, frameSchema } from './time.ts';

/**
 * Spatial transform. x/y are in canvas pixels; the anchor is the item center by default.
 * Scale is multiplicative (scale * scaleX / scaleY). Rotation in degrees, clockwise.
 * No zod `.default()` here on purpose: defaults are applied once by editor-core when items
 * are created; update patches must stay sparse so inverses stay exact.
 */
export const transformSchema = z.object({
  x: z.number(),
  y: z.number(),
  scale: z.number().positive(),
  scaleX: z.number().positive(),
  scaleY: z.number().positive(),
  rotation: z.number(),
  opacity: z.number().min(0).max(1),
});
export type Transform = z.infer<typeof transformSchema>;
export type TransformPatch = Partial<Transform>;

export const DEFAULT_TRANSFORM: Transform = {
  x: 0,
  y: 0,
  scale: 1,
  scaleX: 1,
  scaleY: 1,
  rotation: 0,
  opacity: 1,
};

/** Value params are typed loosely; each effect type declares which keys it reads. */
export const effectParamsSchema = z.record(z.string(), z.union([z.number(), z.string(), z.boolean()]));
export type EffectParams = z.infer<typeof effectParamsSchema>;

export const effectSchema = z.object({
  id: z.string(),
  type: z.string(),
  params: effectParamsSchema,
});
export type Effect = z.infer<typeof effectSchema>;

export const maskShapeSchema = z.enum(['rect', 'ellipse', 'path']);
export const maskSchema = z.object({
  id: z.string(),
  shape: maskShapeSchema,
  /** For `path`: absolute canvas-space points. For rect/ellipse: computed from the item box
   *  with optional insets. */
  path: z.array(z.object({ x: z.number(), y: z.number() })).optional(),
  feather: z.number().min(0).default(0),
  invert: z.boolean().default(false),
  keyframes: z.record(z.string(), z.array(keyframeSchema)).optional(),
  tracking: z
    .object({
      assetId: z.string(),
      status: z.enum(['none', 'queued', 'running', 'done', 'failed']),
      /** sampled positions: [{frame, x, y}] */
      positions: z.array(z.object({ frame: frameSchema, x: z.number(), y: z.number() })).default([]),
    })
    .optional(),
});
export type Mask = z.infer<typeof maskSchema>;
