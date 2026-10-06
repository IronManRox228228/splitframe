import { getDb } from './db.ts';
import { applyOps, createEmptyDoc, parseDoc, History, OpError } from '@cutboard/editor-core';
import type { UndoGroup } from '@cutboard/editor-core';
import { Actor, Op, TimelineDoc, assetSchema, beatMapSchema, sceneSchema, transcriptSchema, newId } from '@cutboard/schema';
import type { ProjectBundle } from '@cutboard/schema';
import { broadcast } from './events.ts';
import { projectDir, getPaths } from './paths.ts';

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

export class ProjectService {
  private current: { id: string; doc: TimelineDoc; dir: string } | null = null;
  private history = new History();

  listRecent(): ProjectRow[] {
    const db = getDb();
    const rows = db
      .prepare(`SELECT id, name, updated_at FROM projects ORDER BY updated_at DESC LIMIT 50`)
      .all() as { id: string; name: string; updated_at: string }[];
    return rows.map((r) => ({ id: r.id, name: r.name, updatedAt: r.updated_at }));
  }

  create(name: string, opts: { fps?: number; width?: number; height?: number } = {}): ProjectRow {
    const db = getDb();
    const id = newId('prj');
    const doc = createEmptyDoc({ id, name, ...opts });
    const now = new Date().toISOString();
    db.prepare(`INSERT INTO projects (id, name, doc, created_at, updated_at) VALUES (?, ?, ?, ?, ?)`).run(
      id,
      name,
      JSON.stringify(doc),
      now,
      now,
    );
    projectDir(getPaths().projectsRoot, id, name); // ensure folder exists
    return { id, name, updatedAt: now };
  }

  open(projectId: string): ProjectBundle {
    const db = getDb();
    const row = db.prepare(`SELECT * FROM projects WHERE id=?`).get(projectId) as
      | { id: string; name: string; doc: string }
      | undefined;
    if (!row) throw new Error(`Project ${projectId} not found`);
    const doc = parseDoc(JSON.parse(row.doc));
    this.current = { id: row.id, doc, dir: projectDir(getPaths().projectsRoot, row.id, row.name) };
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
    const db = getDb();
    const now = new Date().toISOString();
    const info = db
      .prepare(`INSERT INTO op_log (project_id, actor, op, created_at) VALUES (?, ?, ?, ?)`)
      .run(this.current.id, actor, JSON.stringify(ops.length === 1 ? ops[0] : { type: 'batch', ops }), now);
    db.prepare(`UPDATE projects SET doc=?, updated_at=? WHERE id=?`).run(
      JSON.stringify(result.doc),
      now,
      this.current.id,
    );
    this.current.doc = result.doc;
    this.history.push(ops, result.inverse, groupLabel, actor);
    // single source of doc-change events: UI, agent, and MCP edits all flow through here
    broadcast('event', {
      type: 'doc:changed',
      payload: { doc: this.current.doc, seq: Number(info.lastInsertRowid), actor, label: groupLabel ?? null },
    });
    return { inverses: result.inverse, seq: Number(info.lastInsertRowid) };
  }

  undo(): { applied: Op[]; label: string | null } | null {
    if (!this.current) return null;
    const group = this.history.undo();
    if (!group) return null;
    const result = applyOps(this.current.doc, group.inverses, { enforceLocks: false });
    this.persistDoc(result.doc);
    this.current.doc = result.doc;
    return { applied: group.inverses, label: group.label ?? null };
  }

  redo(): { applied: Op[]; label: string | null } | null {
    if (!this.current) return null;
    const group = this.history.redo();
    if (!group) return null;
    const result = applyOps(this.current.doc, group.ops, { enforceLocks: false });
    this.persistDoc(result.doc);
    this.current.doc = result.doc;
    return { applied: group.ops, label: group.label ?? null };
  }

  get historyLabels(): { canUndo: boolean; canRedo: boolean; undoLabel: string | null } {
    return {
      canUndo: this.history.canUndo,
      canRedo: this.history.canRedo,
      undoLabel: this.history.undoLabel,
    };
  }

  private persistDoc(doc: TimelineDoc): void {
    const db = getDb();
    db.prepare(`UPDATE projects SET doc=?, updated_at=? WHERE id=?`).run(
      JSON.stringify(doc),
      new Date().toISOString(),
      this.current!.id,
    );
  }
}

export const projectService = new ProjectService();
export type { UndoGroup };
export { OpError };
