import { z } from 'zod';

export type AssetKind = 'video' | 'audio' | 'image';
export const assetKindSchema = z.enum(['video', 'audio', 'image']);

export type AssetStatus = 'importing' | 'processing' | 'analyzed' | 'failed' | 'missing';
export const assetStatusSchema = z.enum([
  'importing',
  'processing',
  'analyzed',
  'failed',
  'missing',
]);

/**
 * Desktop: originals stay where they are — we store the absolute path plus identity
 * metadata (size, mtime, hash) so moved/missing files can be detected and relinked.
 * `proxyPath` lives in the project folder cache.
 */
export const assetSchema = z.object({
  id: z.string(),
  projectId: z.string(),
  kind: assetKindSchema,
  /** Absolute path of the original file on disk. */
  path: z.string(),
  originalName: z.string(),
  proxyPath: z.string().optional(),
  thumbPath: z.string().optional(),
  waveformPath: z.string().optional(),
  status: assetStatusSchema.default('importing'),
  /** ingest stage detail for progress display */
  stage: z.string().optional(),
  durationMs: z.number().int().nonnegative().default(0),
  width: z.number().int().nonnegative().default(0),
  height: z.number().int().nonnegative().default(0),
  fps: z.number().optional(),
  hasAudio: z.boolean().default(false),
  hasSpeech: z.boolean().default(false),
  sizeBytes: z.number().int().nonnegative().default(0),
  mtimeMs: z.number().default(0),
  hash: z.string().optional(),
  /** Exif-like capture metadata when present */
  metadata: z
    .object({
      location: z.string().optional(),
      capturedAt: z.string().optional(),
      /** video codec reported by ffprobe (lets export use the original when Chromium can decode it) */
      codec: z.string().optional(),
    })
    .default({}),
  createdAt: z.string(),
  error: z.string().optional(),
});
export type Asset = z.infer<typeof assetSchema>;

export const transcriptSchema = z.object({
  assetId: z.string(),
  language: z.string().default('en'),
  words: z.array(
    z.object({
      w: z.string(),
      startMs: z.number().int().nonnegative(),
      endMs: z.number().int().nonnegative(),
      conf: z.number().min(0).max(1).optional(),
      speaker: z.string().optional(),
    }),
  ),
});
export type Transcript = z.infer<typeof transcriptSchema>;

export const sceneSchema = z.object({
  id: z.string(),
  assetId: z.string(),
  startMs: z.number().int().nonnegative(),
  endMs: z.number().int().nonnegative(),
  description: z.string(),
  tags: z.array(z.string()).default([]),
  keyframePaths: z.array(z.string()).default([]),
});
export type Scene = z.infer<typeof sceneSchema>;

export const beatSectionSchema = z.object({
  startMs: z.number().int().nonnegative(),
  endMs: z.number().int().nonnegative(),
  label: z.string(),
  energy: z.number().min(0).max(1).default(0.5),
});

export const beatMapSchema = z.object({
  assetId: z.string(),
  bpm: z.number(),
  beatsMs: z.array(z.number()),
  downbeatsMs: z.array(z.number()),
  sections: z.array(beatSectionSchema),
});
export type BeatMap = z.infer<typeof beatMapSchema>;
