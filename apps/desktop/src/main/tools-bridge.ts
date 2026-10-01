import { Actor } from '@cutboard/schema';
import { createToolRegistry, ToolContext } from '@cutboard/tools';
import { projectService } from './project-service.ts';
import { getAssets, getAsset, getTranscriptsForProject } from './asset-service.ts';
import { renderStill, exportService, EXPORT_PRESETS } from './export-service.ts';
import { editorContextCache } from './editor-context.ts';

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
  };
}

/** Entry point used by the MCP server (and later the built-in chat). */
export async function callTool(name: string, rawInput: unknown, actor: Actor): Promise<unknown> {
  const ctx = makeToolContext(actor);
  return registry.call(name, rawInput, ctx);
}

export { getAsset };
