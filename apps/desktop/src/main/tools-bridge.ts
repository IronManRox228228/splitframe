import { Actor } from '@cutboard/schema';
import { createToolRegistry, ToolContext } from '@cutboard/tools';
import { projectService } from './project-service.ts';
import { getAssets, getAsset, getTranscriptsForProject } from './asset-service.ts';
import { renderStill, exportService, EXPORT_PRESETS } from './export-service.ts';
import { editorContextCache } from './editor-context.ts';
import { detectBeats } from './analysis/beats.ts';
import { getDb } from './db.ts';
import { validateMotionCode } from '@cutboard/renderer';

/**
 * Tool registry bridge (addendum §2): one registry, two front doors — the built-in chat
 * and the MCP server both call through here. Handlers run in the main process against
 * the authoritative project state.
 */

export interface ProjectSnapshot {
  doc: unknown;
  assets: unknown[];
  transcripts: unknown[];
  scenes: unknown[];
  beatMaps: unknown[];
  editorContext: unknown;
}

export const registry = createToolRegistry();

export function makeToolContext(actor: Actor): ToolContext {
  return {
    actor,
    applyOps: async (ops, a, label) => projectService.apply(ops, a ?? actor, label),
    async getSnapshot(): Promise<ProjectSnapshot> {
      const doc = projectService.doc;
      const projectId = projectService.projectId;
      return {
        doc,
        assets: getAssets(projectId),
        transcripts: getTranscriptsForProject(projectId),
        scenes: [],
        beatMaps: [],
        editorContext: editorContextCache.get(),
      };
    },
    captureFrame: (frame, width) => renderStill(frame, width),
    startExport: (presetName) => exportService.start(presetName),
    getExportStatus: async (exportId) => exportService.get(exportId) ?? { exportId, status: 'unknown' },
    listExportPresets: async () => EXPORT_PRESETS,
    analyzeBeats: async (assetId) => {
      // cached in the beat_maps table; detected on demand otherwise
      const db = getDb();
      const row = db.prepare(`SELECT * FROM beat_maps WHERE asset_id=?`).get(assetId) as Record<string, unknown> | undefined;
      if (row && row.beats) {
        return {
          bpm: (row.bpm as number) ?? 120,
          beatsMs: JSON.parse((row.beats as string) ?? '[]'),
          downbeatsMs: JSON.parse((row.downbeats as string) ?? '[]'),
          sections: JSON.parse((row.sections as string) ?? '[]'),
        };
      }
      const asset = getAsset(assetId);
      if (!asset) throw new Error(`Asset ${assetId} not found.`);
      const map = await detectBeats(asset.path);
      db.prepare(`INSERT OR REPLACE INTO beat_maps (asset_id, bpm, beats, downbeats, sections) VALUES (?, ?, ?, ?, ?)`).run(
        assetId,
        map.bpm,
        JSON.stringify(map.beatsMs),
        JSON.stringify(map.downbeatsMs),
        JSON.stringify(map.sections),
      );
      return map;
    },
    validateMotion: (code) => validateMotionCode(code),
  };
}

/** Entry point used by the MCP server (and later the built-in chat). */
export async function callTool(name: string, rawInput: unknown, actor: Actor): Promise<unknown> {
  const ctx = makeToolContext(actor);
  return registry.call(name, rawInput, ctx);
}

export { getAsset };
