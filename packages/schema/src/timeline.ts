import { z } from 'zod';
import { projectSchema, trackSchema, markerSchema } from './project.ts';
import { itemSchema } from './item.ts';
import { Asset, BeatMap, Scene, Transcript, assetSchema, beatMapSchema, sceneSchema, transcriptSchema } from './asset.ts';

export const timelineDocSchema = z.object({
  project: projectSchema,
  tracks: z.array(trackSchema),
  items: z.array(itemSchema),
  markers: z.array(markerSchema),
});
export type TimelineDoc = z.infer<typeof timelineDocSchema>;

/** Everything the main process knows about one project, including analysis state. */
export const projectBundleSchema = z.object({
  doc: timelineDocSchema,
  assets: z.array(assetSchema),
  transcripts: z.array(transcriptSchema),
  scenes: z.array(sceneSchema),
  beatMaps: z.array(beatMapSchema),
});
export type ProjectBundle = z.infer<typeof projectBundleSchema>;
export type { Asset, BeatMap, Scene, Transcript };
