import { z } from 'zod';
import type { ToolDef } from '../registry.ts';

export const searchFederatedMedia: ToolDef = {
  name: 'searchFederatedMedia',
  description:
    'Search footage, B-roll, and scene catalogs federated across storage pools (local NVMe, studio NAS, cloud S3/GCS) via Zetaris. Returns matching clips with descriptions, tags, resolutions, and relevance scores.',
  input: z.object({
    query: z.string().describe('Natural language search description (e.g. "drone aerial sunset over bridge", "tokyo street neon")'),
    tags: z.array(z.string()).optional().describe('Specific visual or topic tags to filter by'),
    storage: z.enum(['local', 'studio_nas', 'cloud_s3', 'cloud_gcs', 'all']).optional().describe('Filter by storage pool'),
    minWidth: z.number().int().optional().describe('Minimum video width in px (e.g. 1920 or 3840)'),
    limit: z.number().int().min(1).max(100).default(20).optional().describe('Maximum number of clips to return'),
  }),
  mutates: false,
  async handler(input, ctx) {
    if (!ctx.searchFederatedMedia) throw new Error('Federated catalog search is unavailable.');
    const storageFilter = input.storage && input.storage !== 'all' ? [input.storage] : undefined;
    const results = await ctx.searchFederatedMedia({
      naturalLanguage: input.query,
      filterTags: input.tags,
      storageFilter,
      minWidth: input.minWidth,
      limit: input.limit ?? 20,
    });
    return { query: input.query, count: results.length, assets: results };
  },
};

export const importFederatedAsset: ToolDef = {
  name: 'importFederatedAsset',
  description:
    'Import or reference a federated asset found via searchFederatedMedia into the project media pool so it can be placed onto timeline tracks.',
  input: z.object({
    assetId: z.string().describe('The federated asset id to import'),
    uri: z.string().describe('Storage URI of the asset (file://, smb://, s3://, gcs://)'),
    name: z.string().describe('Display name of the asset'),
    storageType: z.enum(['local', 'studio_nas', 'cloud_s3', 'cloud_gcs', 'unknown']).optional().default('local'),
    description: z.string().optional().default(''),
    tags: z.array(z.string()).optional().default([]),
  }),
  mutates: true,
  async handler(input, ctx) {
    if (!ctx.importFederatedAsset) throw new Error('Federated asset import is unavailable.');
    const res = await ctx.importFederatedAsset(input);
    return res;
  },
};

export const ZETARIS_TOOLS: ToolDef[] = [searchFederatedMedia, importFederatedAsset];
