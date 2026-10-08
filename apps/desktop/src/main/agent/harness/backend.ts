import { existsSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import type { Op, Transcript } from '@cutboard/schema';
import { editorContextCache } from '../../editor-context.ts';
import { getAssets, getFootageNotes, getSilenceMap, getTranscriptsForProject } from '../../asset-service.ts';
import { projectService } from '../../project-service.ts';
import { callTool } from '../../tools-bridge.ts';
import type { AssetNotes, Backend } from './types.ts';

/** The harness backend over the real project service and the classic tool registry. */

const PLAN_FILE = 'agent-plan.json';

export function appBackend(actor: 'builtin-agent' = 'builtin-agent'): Backend {
  return {
    async snapshot() {
      const ec = editorContextCache.get();
      return {
        doc: structuredClone(projectService.doc),
        assets: getAssets(projectService.projectId),
        transcripts: getTranscriptsForProject(projectService.projectId) as Transcript[],
        editor: { selection: ec.selection, playheadFrame: ec.playheadFrame, highlightedRange: ec.highlightedRange ?? null },
      };
    },
    call: (tool, args) => callTool(tool, args, actor),
    async apply(ops: Op[], label: string) {
      projectService.apply(ops, actor, label);
    },
    silences: (assetId) => getSilenceMap(assetId),
    notes(assetId): AssetNotes | null {
      const n = getFootageNotes(assetId);
      if (!n || n.segments.length === 0) return null;
      const issues = [...new Set(n.segments.flatMap((s) => s.issues))].filter((i) => i !== 'silent');
      const text = [...new Set(n.segments.map((s) => (s.text ?? '').trim()).filter(Boolean))];
      const scenes = new Set(n.segments.map((s) => s.sceneIndex)).size;
      const quality = Math.round(n.segments.reduce((a, s) => a + s.quality, 0) / n.segments.length);
      return { scenes, issues, text, quality, integratedLufs: n.integratedLufs };
    },
    beginGroup: (label) => projectService.beginUndoGroup(label, actor),
    endGroup: () => projectService.endUndoGroup(),
    loadPlan() {
      try {
        const path = join(projectService.dir, PLAN_FILE);
        return existsSync(path) ? JSON.parse(readFileSync(path, 'utf8')) : null;
      } catch {
        return null;
      }
    },
    savePlan(plan) {
      try {
        const path = join(projectService.dir, PLAN_FILE);
        if (plan === null) rmSync(path, { force: true });
        else writeFileSync(path, JSON.stringify(plan, null, 2));
      } catch {
        /* the plan is a convenience; a failed save must not fail the edit */
      }
    },
  };
}
