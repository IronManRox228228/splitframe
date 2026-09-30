import { z } from 'zod';
import { frameSchema, keyframeSchema } from './time.ts';
import { effectSchema, maskSchema, transformSchema } from './transform.ts';

export const itemTypeSchema = z.enum([
  'video',
  'audio',
  'image',
  'text',
  'caption',
  'shape',
  'motionGraphic',
]);
export type ItemType = z.infer<typeof itemTypeSchema>;

/** Types that draw visuals (everything except audio). */
export const VISUAL_TYPES: readonly ItemType[] = [
  'video',
  'image',
  'text',
  'caption',
  'shape',
  'motionGraphic',
];

export const textStyleSchema = z.object({
  fontFamily: z.string(),
  fontSize: z.number().positive(),
  fontWeight: z.number().int().min(100).max(1000).default(700),
  color: z.string(),
  strokeColor: z.string().optional(),
  strokeWidth: z.number().min(0).default(0),
  backgroundColor: z.string().optional(),
  align: z.enum(['left', 'center', 'right']).default('center'),
  lineHeight: z.number().positive().default(1.2),
  letterSpacing: z.number().default(0),
  uppercase: z.boolean().default(false),
  padding: z.number().min(0).default(0),
  borderRadius: z.number().min(0).default(0),
});
export type TextStyle = z.infer<typeof textStyleSchema>;

export const captionStyleSchema = textStyleSchema.extend({
  /** Karaoke highlight mode for the active word. */
  highlight: z.enum(['none', 'active-word', 'word-bg']).default('active-word'),
  highlightColor: z.string().default('#fbbf24'),
  /** vertical placement: 0 = top of canvas, 1 = bottom */
  placementY: z.number().min(0).max(1).default(0.82),
  maxCharsPerLine: z.number().int().positive().default(28),
});
export type CaptionStyle = z.infer<typeof captionStyleSchema>;

export const transcriptWordSchema = z.object({
  w: z.string(),
  startMs: z.number().int().nonnegative(),
  endMs: z.number().int().nonnegative(),
  conf: z.number().min(0).max(1).optional(),
  speaker: z.string().optional(),
});
export type TranscriptWord = z.infer<typeof transcriptWordSchema>;

export const shapePropsSchema = z.object({
  shape: z.enum(['rect', 'ellipse', 'triangle']),
  fill: z.string(),
  stroke: z.string().optional(),
  strokeWidth: z.number().min(0).default(0),
  radius: z.number().min(0).default(0),
  /** logical size within the canvas; transform scales it */
  width: z.number().positive(),
  height: z.number().positive(),
});

export const timeRemapPointSchema = z.object({
  frame: frameSchema, // timeline frame, item-local
  sourceFrame: frameSchema, // source-local
});

/**
 * Props are per-item-type and stored whole (update patches replace the whole `props`
 * object). Keeping them whole makes inverse patches trivial and the tool layer is
 * responsible for composing complete props.
 */
export const itemPropsSchemas = {
  video: z.object({ fadeInFrames: frameSchema.default(0), fadeOutFrames: frameSchema.default(0) }),
  audio: z.object({ fadeInFrames: frameSchema.default(0), fadeOutFrames: frameSchema.default(0) }),
  image: z.object({}),
  text: z.object({ text: z.string(), style: textStyleSchema }),
  caption: z.object({
    words: z.array(transcriptWordSchema),
    style: captionStyleSchema,
    mode: z.enum(['word', 'phrase']).default('phrase'),
    maxWordsPerCard: z.number().int().positive().default(5),
  }),
  shape: shapePropsSchema,
  /** Code is validated + sandboxed at runtime (main prompt §7); stored verbatim here. */
  motionGraphic: z.object({
    code: z.string(),
    inputProps: z.record(z.string(), z.unknown()).default({}),
  }),
} as const;

export type ItemPropsMap = {
  [K in keyof typeof itemPropsSchemas]: z.infer<(typeof itemPropsSchemas)[K]>;
};

const itemBase = {
  id: z.string(),
  trackId: z.string(),
  startFrame: frameSchema.nonnegative(),
  durationFrames: frameSchema.min(1),
  assetId: z.string().optional(),
  /** Source-local in point; derived out = sourceIn + durationFrames * speed. */
  sourceInFrame: frameSchema.nonnegative().optional(),
  /** Frames per source frame; 2 = double speed. Ignored when timeRemap is non-empty. */
  speed: z.number().min(0.1).max(16).default(1),
  timeRemap: z.array(timeRemapPointSchema).default([]),
  transform: transformSchema,
  volume: z.number().min(0).default(1),
  muted: z.boolean().default(false),
  effects: z.array(effectSchema).default([]),
  masks: z.array(maskSchema).default([]),
  keyframes: z.record(z.string(), z.array(keyframeSchema)).default({}),
  labels: z
    .object({
      name: z.string().optional(),
      color: z.string().optional(),
    })
    .default({}),
};

export const itemSchemas = {
  video: z.object({ ...itemBase, type: z.literal('video'), props: itemPropsSchemas.video }),
  audio: z.object({ ...itemBase, type: z.literal('audio'), props: itemPropsSchemas.audio }),
  image: z.object({ ...itemBase, type: z.literal('image'), props: itemPropsSchemas.image }),
  text: z.object({ ...itemBase, type: z.literal('text'), props: itemPropsSchemas.text }),
  caption: z.object({ ...itemBase, type: z.literal('caption'), props: itemPropsSchemas.caption }),
  shape: z.object({ ...itemBase, type: z.literal('shape'), props: itemPropsSchemas.shape }),
  motionGraphic: z.object({
    ...itemBase,
    type: z.literal('motionGraphic'),
    props: itemPropsSchemas.motionGraphic,
  }),
} as const;

export const itemSchema = z.discriminatedUnion('type', [
  itemSchemas.video,
  itemSchemas.audio,
  itemSchemas.image,
  itemSchemas.text,
  itemSchemas.caption,
  itemSchemas.shape,
  itemSchemas.motionGraphic,
]);
export type Item = z.infer<typeof itemSchema>;
export type ItemOfType<K extends ItemType> = z.infer<(typeof itemSchemas)[K]>;

/** Loose patch used by item.update — validated against the item's type where needed. */
export const itemPatchSchema = z.object({
  trackId: z.string().optional(),
  startFrame: frameSchema.optional(),
  durationFrames: frameSchema.min(1).optional(),
  sourceInFrame: frameSchema.nonnegative().optional(),
  speed: z.number().min(0.1).max(16).optional(),
  timeRemap: z.array(timeRemapPointSchema).optional(),
  transform: transformSchema.partial().optional(),
  volume: z.number().min(0).optional(),
  muted: z.boolean().optional(),
  effects: z.array(effectSchema).optional(),
  masks: z.array(maskSchema).optional(),
  keyframes: z.record(z.string(), z.array(keyframeSchema)).optional(),
  labels: z
    .object({ name: z.string().optional(), color: z.string().optional() })
    .optional(),
  /** Whole-replace of the type-specific props. Shape-checked against the item's type. */
  props: z.unknown().optional(),
});
export type ItemPatch = z.infer<typeof itemPatchSchema>;
