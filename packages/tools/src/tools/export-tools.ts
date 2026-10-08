import { z } from 'zod';
import type { ToolDef } from '../registry.ts';

/** Export tools: agents can export unattended and poll progress. */

export const exportVideo: ToolDef = {
  name: 'exportVideo',
  description:
    'Start an export with a preset: a platform preset (TikTok / Reels / Shorts, YouTube 1080p, Square 1080, 1440p, WebM 1080p) or a quality tier "720p", "1080p", "1440p" (short side of the canvas; add " WebM" for WebM). Returns an exportId; poll getExportStatus. Frame-accurate: output matches captureFrame.',
  input: z.object({
    preset: z.string().default('YouTube 1080p').describe('Preset name from listExportPresets, e.g. "720p"'),
  }),
  mutates: false,
  async handler(input, ctx) {
    const row = (await ctx.startExport(input.preset)) as { id: string; status: string };
    return { exportId: row.id, status: row.status, note: 'Poll getExportStatus with this exportId.' };
  },
};

export const getExportStatus: ToolDef = {
  name: 'getExportStatus',
  description: 'Check export progress/status: queued → rendering → encoding → done (with output path) or failed (with error).',
  input: z.object({ exportId: z.string() }),
  mutates: false,
  async handler(input, ctx) {
    return ctx.getExportStatus(input.exportId);
  },
};

export const listExportPresets: ToolDef = {
  name: 'listExportPresets',
  description: 'List export presets (resolution/format/bitrate) available on this machine.',
  input: z.object({}),
  mutates: false,
  async handler(_input, ctx) {
    return ctx.listExportPresets();
  },
};

export const EXPORT_TOOLS: ToolDef[] = [exportVideo, getExportStatus, listExportPresets];
