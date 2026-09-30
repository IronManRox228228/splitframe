import { z } from 'zod';
import { captionStyleSchema, textStyleSchema } from './item.ts';

export const trackKindSchema = z.enum(['video', 'audio', 'overlay', 'text']);
export type TrackKind = z.infer<typeof trackKindSchema>;

/**
 * Tracks are stored top-first: index 0 renders last (on top). Video items live on
 * `video`/`overlay` tracks, text/caption/motionGraphic/shape on `text` tracks,
 * audio-bearing items on `audio` tracks.
 */
export const trackSchema = z.object({
  id: z.string(),
  kind: trackKindSchema,
  name: z.string(),
  locked: z.boolean().default(false),
  muted: z.boolean().default(false),
  hidden: z.boolean().default(false),
});
export type Track = z.infer<typeof trackSchema>;

export const styleConfigSchema = z.object({
  fonts: z.array(z.string()).default([]),
  primaryColor: z.string().default('#fbbf24'),
  backgroundColor: z.string().default('#0a0a0a'),
  captionStyle: captionStyleSchema.optional(),
  titleStyle: textStyleSchema.optional(),
});
export type StyleConfig = z.infer<typeof styleConfigSchema>;

export const projectSchema = z.object({
  id: z.string(),
  name: z.string(),
  fps: z.number().int().positive().default(30),
  width: z.number().int().positive().default(1920),
  height: z.number().int().positive().default(1080),
  templateId: z.string().optional(),
  styleConfig: styleConfigSchema.default({
    fonts: [],
    primaryColor: '#fbbf24',
    backgroundColor: '#0a0a0a',
  }),
  /** Reference video asset for style matching (main prompt §1.2); never used in output. */
  referenceAssetId: z.string().optional(),
  createdAt: z.string(),
  updatedAt: z.string(),
});
export type Project = z.infer<typeof projectSchema>;

export const markerSchema = z.object({
  id: z.string(),
  frame: z.number().int().nonnegative(),
  label: z.string(),
  color: z.string().optional(),
});
export type Marker = z.infer<typeof markerSchema>;
