import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { getDb } from './db.ts';
import { applyOps, createEmptyDoc, parseDoc, History, OpError } from '@cutboard/editor-core';
import type { UndoGroup } from '@cutboard/editor-core';
import { Actor, Op, TimelineDoc, assetSchema, beatMapSchema, sceneSchema, transcriptSchema, newId } from '@cutboard/schema';
import type { ProjectBundle } from '@cutboard/schema';
import { broadcast } from './events.ts';
import { projectDir, getPaths } from './paths.ts';
import { commitProjectDoc, type SqlDb } from './project-store.ts';
import {
  DEFAULT_PROJECT_NAME,
  deleteProject as deleteProjectRows,
  duplicateProject as duplicateProjectRows,
  findProjectDir,
  listProjects,
  renameProject as renameProjectRows,
  uniqueProjectName,
  type LibDb,
  type ProjectSummary,
} from './project-library.ts';
import { copySceneVectors, indexTranscriptWords, removeAssetIndex } from './analysis/search.ts';
import { jobs } from './jobs.ts';

/**
 * The main process is the source of truth (addendum §2). Every timeline change goes
 * through here: zod-validated op → editor-core apply → append-only op log → doc
 * snapshot update. Undo/redo is a History of {ops, inverses} groups.
 */
export interface ProjectRow {
  id: string;
  name: string;
  updatedAt: string;
}
export type { ProjectSummary };

/** Folder of any project (not only the open one), e.g. for background jobs of another project. */
export function resolveProjectDir(projectId: string): string {
  const row = getDb().prepare(`SELECT id, name FROM projects WHERE id=?`).get(projectId) as { id: string; name: string } | undefined;
  if (!row) throw new Error(`Project ${projectId} not found`);
  return dirOf(row.id, row.name);
}

/** Existing folder of a project (found by id, so renames don't orphan it), or a fresh one. */
function dirOf(id: string, name: string): string {
  const root = getPaths().projectsRoot;
  const existing = findProjectDir(root, id);
  if (existing) {
    mkdirSync(join(existing, 'cache'), { recursive: true });
    mkdirSync(join(existing, 'exports'), { recursive: true });
    return existing;
  }
  return projectDir(root, id, name);
}


/** Rebuild search rows for freshly copied assets: transcript words and scene embeddings. */
function reindexCopiedAssets(
  copies: { fromAssetId: string; toAssetId: string; sceneIds: { from: string; to: string }[] }[],
): void {
  const db = getDb();
  for (const c of copies) {
    const row = db.prepare(`SELECT words FROM transcripts WHERE asset_id=?`).get(c.toAssetId) as { words: string } | undefined;
    if (row) indexTranscriptWords(c.toAssetId, JSON.parse(row.words));
    copySceneVectors(c.sceneIds);
  }
}

export class ProjectService {
  private current: { id: string; doc: TimelineDoc; dir: string } | null = null;
  private history = new History();

  /** Every project, newest first, with the summary fields the Home grid shows. */
  listRecent(): ProjectSummary[] {
    return listProjects(getDb() as unknown as LibDb);
  }

  create(name: string | undefined, opts: { fps?: number; width?: number; height?: number } = {}): ProjectRow {
    const db = getDb();
    const finalName = name?.trim() ? name.trim() : uniqueProjectName(db as unknown as LibDb, DEFAULT_PROJECT_NAME);
    const id = newId('prj');
    const doc = createEmptyDoc({ id, name: finalName, ...opts });
    const now = new Date().toISOString();
    db.prepare(`INSERT INTO projects (id, name, doc, created_at, updated_at) VALUES (?, ?, ?, ?, ?)`).run(
      id,
      finalName,
      JSON.stringify(doc),
      now,
      now,
    );
    projectDir(getPaths().projectsRoot, id, finalName); // ensure folder exists
    return { id, name: finalName, updatedAt: now };
  }

  rename(projectId: string, name: string): void {
    if (this.current?.id === projectId) {
      // the open project renames through the op pipeline so the editor, undo and op log stay in step
      this.apply([{ type: 'project.rename', name: name.trim() }], 'user', 'Rename project');
      return;
    }
    renameProjectRows(getDb() as unknown as LibDb, projectId, name);
  }

  duplicate(projectId: string): ProjectRow {
    const db = getDb();
    const res = duplicateProjectRows(db as unknown as LibDb, {
      projectsRoot: getPaths().projectsRoot,
      projectId,
      dirFor: (id, name) => projectDir(getPaths().projectsRoot, id, name),
      // the copy has new asset ids, so its transcript words and scene vectors need their own index rows
      onAssetsCopied: reindexCopiedAssets,
    });
    return { id: res.id, name: res.name, updatedAt: new Date().toISOString() };
  }

  /** Remove a project and its folder. The open project is refused: close it first. */
  remove(projectId: string): void {
    if (this.current?.id === projectId) throw new Error('Close this project before deleting it.');
    for (const job of jobs.list(projectId)) {
      if (job.status === 'pending' || job.status === 'running') jobs.cancel(job.id);
    }
    deleteProjectRows(getDb() as unknown as LibDb, {
      projectsRoot: getPaths().projectsRoot,
      projectId,
      onAssetsRemoved: (assetIds, sceneIds) => {
        assetIds.forEach((id, i) => removeAssetIndex(id, i === 0 ? sceneIds : []));
      },
    });
  }

  /** Folder of any project, for "Show in folder". */
  dirOfProject(projectId: string): string {
    return resolveProjectDir(projectId);
  }

  open(projectId: string): ProjectBundle {
    const db = getDb();
    const row = db.prepare(`SELECT * FROM projects WHERE id=?`).get(projectId) as
      | { id: string; name: string; doc: string }
      | undefined;
    if (!row) throw new Error(`Project ${projectId} not found`);
    const doc = parseDoc(JSON.parse(row.doc));
    this.current = { id: row.id, doc, dir: dirOf(row.id, row.name) };
    this.history.clear();

    const assets = (db.prepare(`SELECT * FROM assets WHERE project_id=? ORDER BY created_at`).all(projectId) as Record<string, unknown>[]).map(
      (r) =>
        assetSchema.parse({
          id: r.id,
          projectId: r.project_id,
          kind: r.kind,
          path: r.path,
          originalName: r.original_name,
          proxyPath: r.proxy_path ?? undefined,
          thumbPath: r.thumb_path ?? undefined,
          waveformPath: r.waveform_path ?? undefined,
          status: r.status,
          stage: r.stage ?? undefined,
          durationMs: r.duration_ms,
          width: r.width,
          height: r.height,
          fps: r.fps ?? undefined,
          hasAudio: Boolean(r.has_audio),
          hasSpeech: Boolean(r.has_speech),
          sizeBytes: r.size_bytes,
          mtimeMs: r.mtime_ms,
          hash: r.hash ?? undefined,
          metadata: JSON.parse((r.metadata as string) ?? '{}'),
          createdAt: r.created_at,
          error: r.error ?? undefined,
        }),
    );
    const transcripts = (
      db.prepare(`SELECT * FROM transcripts WHERE asset_id IN (SELECT id FROM assets WHERE project_id=?)`).all(projectId) as Record<string, unknown>[]
    ).map((r) =>
      transcriptSchema.parse({ assetId: r.asset_id, language: r.language, words: JSON.parse(r.words as string) }),
    );
    const scenes = (
      db.prepare(`SELECT * FROM scenes WHERE asset_id IN (SELECT id FROM assets WHERE project_id=?)`).all(projectId) as Record<string, unknown>[]
    ).map((r) =>
      sceneSchema.parse({
        id: r.id,
        assetId: r.asset_id,
        startMs: r.start_ms,
        endMs: r.end_ms,
        description: r.description,
        tags: JSON.parse(r.tags as string),
        keyframePaths: JSON.parse(r.keyframe_paths as string),
      }),
    );
    const beatMaps = (
      db.prepare(`SELECT * FROM beat_maps WHERE asset_id IN (SELECT id FROM assets WHERE project_id=?)`).all(projectId) as Record<string, unknown>[]
    ).map((r) =>
      beatMapSchema.parse({
        assetId: r.asset_id,
        bpm: r.bpm,
        beats: JSON.parse(r.beats as string),
        downbeats: JSON.parse(r.downbeats as string),
        sections: JSON.parse(r.sections as string),
      }),
    );
    return { doc, assets, transcripts, scenes, beatMaps };
  }

  close(): void {
    this.current = null;
    this.history.clear();
  }

  get isOpen(): boolean {
    return this.current !== null;
  }

  get projectId(): string {
    if (!this.current) throw new Error('No project open');
    return this.current.id;
  }

  get doc(): TimelineDoc {
    if (!this.current) throw new Error('No project open');
    return this.current.doc;
  }

  get dir(): string {
    if (!this.current) throw new Error('No project open');
    return this.current.dir;
  }

  /** Apply ops from any actor (UI, agent, MCP). Returns inverses for undo. */
  apply(ops: Op[], actor: Actor, groupLabel?: string): { inverses: Op[]; seq: number } {
    if (!this.current) throw new Error('No project open');
    const result = applyOps(this.current.doc, ops);
    const seq = this.persist(result.doc, { actor, ops });
    this.current.doc = result.doc;
    this.history.push(ops, result.inverse, groupLabel, actor);
    // single source of doc-change events: UI, agent, and MCP edits all flow through here
    broadcast('event', {
      type: 'doc:changed',
      payload: { doc: this.current.doc, seq, actor, label: groupLabel ?? null },
    });
    return { inverses: result.inverse, seq };
  }

  undo(): { applied: Op[]; label: string | null } | null {
    if (!this.current) return null;
    const current = this.current;
    // undoWith puts the history position back if applying throws, so history and doc stay in step
    return this.history.undoWith((group) => {
      const result = applyOps(current.doc, group.inverses, { enforceLocks: false });
      this.persist(result.doc, { actor: 'user', ops: group.inverses });
      current.doc = result.doc;
      return { applied: group.inverses, label: group.label ?? null };
    });
  }

  redo(): { applied: Op[]; label: string | null } | null {
    if (!this.current) return null;
    const current = this.current;
    return this.history.redoWith((group) => {
      const result = applyOps(current.doc, group.ops, { enforceLocks: false });
      this.persist(result.doc, { actor: 'user', ops: group.ops });
      current.doc = result.doc;
      return { applied: group.ops, label: group.label ?? null };
    });
  }

  /** An asset was deleted for good: undo/redo must not bring back clips that point at it. */
  forgetAsset(assetId: string): void {
    this.history.dropReferences(assetId);
  }

  get historyLabels(): { canUndo: boolean; canRedo: boolean; undoLabel: string | null } {
    return {
      canUndo: this.history.canUndo,
      canRedo: this.history.canRedo,
      undoLabel: this.history.undoLabel,
    };
  }

  /** Op-log row + doc snapshot in one transaction; undo/redo log the ops they applied, so the log replays to the current doc. */
  private persist(doc: TimelineDoc, log: { actor: string; ops: Op[] }): number {
    return commitProjectDoc(getDb() as unknown as SqlDb, {
      projectId: this.current!.id,
      doc,
      log,
      now: new Date().toISOString(),
    });
  }
}

export const projectService = new ProjectService();
export type { UndoGroup };
export { OpError };
