import { cpSync, existsSync, readdirSync, rmSync } from 'node:fs';
import { basename, dirname, isAbsolute, join, relative, resolve } from 'node:path';
import { newId } from '@cutboard/schema';

/**
 * Library-level project operations (list / rename / duplicate / delete). Kept free of
 * Electron imports and written against a tiny SQL interface so it can be unit tested.
 */

export interface LibStatement {
  all(...args: unknown[]): unknown[];
  get(...args: unknown[]): unknown;
  run(...args: unknown[]): unknown;
}
export interface LibDb {
  prepare(sql: string): LibStatement;
  transaction<T>(fn: () => T): () => T;
}

export interface ProjectSummary {
  id: string;
  name: string;
  updatedAt: string;
  width: number;
  height: number;
  fps: number;
  durationMs: number;
  /** absolute path of the first video/image asset's thumbnail, if the project has one */
  thumbPath: string | null;
}

export const DEFAULT_PROJECT_NAME = 'Untitled video';
export const MAX_NAME_LENGTH = 120;

/** The folder suffix every project directory ends with (see paths.projectDir). */
export const dirSuffix = (projectId: string): string => `-${projectId.slice(4, 12)}`;

export function listProjects(db: LibDb): ProjectSummary[] {
  const rows = db.prepare(`SELECT id, name, doc, updated_at FROM projects ORDER BY updated_at DESC`).all() as {
    id: string;
    name: string;
    doc: string;
    updated_at: string;
  }[];
  const thumbQuery = db.prepare(
    `SELECT thumb_path FROM assets WHERE project_id=? AND kind IN ('video','image') AND thumb_path IS NOT NULL AND thumb_path != '' ORDER BY created_at LIMIT 1`,
  );
  return rows.map((r) => {
    let width = 1920;
    let height = 1080;
    let fps = 30;
    let frames = 0;
    try {
      const doc = JSON.parse(r.doc) as {
        project?: { width?: number; height?: number; fps?: number };
        items?: { startFrame?: number; durationFrames?: number }[];
      };
      width = doc.project?.width ?? width;
      height = doc.project?.height ?? height;
      fps = doc.project?.fps || fps;
      for (const it of doc.items ?? []) frames = Math.max(frames, (it.startFrame ?? 0) + (it.durationFrames ?? 0));
    } catch {
      /* unreadable doc: show defaults rather than hide the project */
    }
    const thumb = thumbQuery.get(r.id) as { thumb_path: string } | undefined;
    return {
      id: r.id,
      name: r.name,
      updatedAt: r.updated_at,
      width,
      height,
      fps,
      durationMs: Math.round((frames / fps) * 1000),
      thumbPath: thumb?.thumb_path ?? null,
    };
  });
}

/** "Untitled video", then "Untitled video 2", "Untitled video 3"... */
export function uniqueProjectName(db: LibDb, base: string = DEFAULT_PROJECT_NAME): string {
  const taken = new Set((db.prepare(`SELECT name FROM projects`).all() as { name: string }[]).map((r) => r.name.toLowerCase()));
  if (!taken.has(base.toLowerCase())) return base;
  for (let n = 2; ; n++) {
    const candidate = `${base} ${n}`;
    if (!taken.has(candidate.toLowerCase())) return candidate;
  }
}

/** Rename the DB row and the name inside the stored doc (no folder is touched: folders are found by id). */
export function renameProject(db: LibDb, projectId: string, name: string, now = new Date().toISOString()): void {
  const clean = name.trim();
  if (!clean) throw new Error('A project needs a name.');
  if (clean.length > MAX_NAME_LENGTH) throw new Error(`Project names are limited to ${MAX_NAME_LENGTH} characters.`);
  const row = db.prepare(`SELECT doc FROM projects WHERE id=?`).get(projectId) as { doc: string } | undefined;
  if (!row) throw new Error(`Project ${projectId} not found`);
  const doc = JSON.parse(row.doc) as { project: { name: string } };
  doc.project.name = clean;
  db.prepare(`UPDATE projects SET name=?, doc=?, updated_at=? WHERE id=?`).run(clean, JSON.stringify(doc), now, projectId);
}

/**
 * The folder of a project, found by its id suffix so renaming a project never orphans its
 * cache. Returns null when no folder exists on disk (yet).
 */
export function findProjectDir(projectsRoot: string, projectId: string): string | null {
  if (!existsSync(projectsRoot)) return null;
  const suffix = dirSuffix(projectId);
  const hit = readdirSync(projectsRoot, { withFileTypes: true }).find((d) => d.isDirectory() && d.name.endsWith(suffix));
  return hit ? join(projectsRoot, hit.name) : null;
}

function isInside(root: string, target: string): boolean {
  const rel = relative(resolve(root), resolve(target));
  return rel !== '' && !rel.startsWith('..') && !isAbsolute(rel);
}

function remapPath(p: string | null | undefined, from: string | null, to: string): string | null {
  if (!p) return p ?? null;
  if (from && isInside(from, p)) return join(to, relative(resolve(from), resolve(p)));
  return p;
}

const insertRow = (db: LibDb, table: string, row: Record<string, unknown>) => {
  const cols = Object.keys(row);
  db.prepare(`INSERT INTO ${table} (${cols.join(', ')}) VALUES (${cols.map(() => '?').join(', ')})`).run(...cols.map((c) => row[c]));
};

/**
 * Copy a project: doc, assets (new ids, so the two projects never share rows), transcripts,
 * scenes, beat maps and the generated cache. Original media files are referenced, not copied.
 */
export function duplicateProject(
  db: LibDb,
  opts: { projectsRoot: string; projectId: string; dirFor(projectId: string, name: string): string; now?: string },
): { id: string; name: string } {
  const { projectsRoot, projectId } = opts;
  const now = opts.now ?? new Date().toISOString();
  const src = db.prepare(`SELECT * FROM projects WHERE id=?`).get(projectId) as { name: string; doc: string; created_at: string } | undefined;
  if (!src) throw new Error(`Project ${projectId} not found`);
  const name = uniqueProjectName(db, `${src.name} copy`.slice(0, MAX_NAME_LENGTH));
  const newProjectId = newId('prj');
  const srcDir = findProjectDir(projectsRoot, projectId);
  const dstDir = opts.dirFor(newProjectId, name);
  if (srcDir && existsSync(join(srcDir, 'cache'))) cpSync(join(srcDir, 'cache'), join(dstDir, 'cache'), { recursive: true });

  const doc = JSON.parse(src.doc) as {
    project: { id: string; name: string; createdAt?: string; updatedAt?: string };
    items: { assetId?: string }[];
  };
  doc.project.id = newProjectId;
  doc.project.name = name;
  doc.project.createdAt = now;
  doc.project.updatedAt = now;

  const assetIdMap = new Map<string, string>();
  const run = db.transaction(() => {
    const assets = db.prepare(`SELECT * FROM assets WHERE project_id=?`).all(projectId) as Record<string, unknown>[];
    for (const a of assets) assetIdMap.set(a.id as string, newId('ast'));
    for (const it of doc.items) if (it.assetId && assetIdMap.has(it.assetId)) it.assetId = assetIdMap.get(it.assetId);
    insertRow(db, 'projects', { id: newProjectId, name, doc: JSON.stringify(doc), created_at: now, updated_at: now });
    for (const a of assets) {
      insertRow(db, 'assets', {
        ...a,
        id: assetIdMap.get(a.id as string),
        project_id: newProjectId,
        proxy_path: remapPath(a.proxy_path as string | null, srcDir, dstDir),
        thumb_path: remapPath(a.thumb_path as string | null, srcDir, dstDir),
        waveform_path: remapPath(a.waveform_path as string | null, srcDir, dstDir),
      });
      const newAsset = assetIdMap.get(a.id as string)!;
      const transcript = db.prepare(`SELECT * FROM transcripts WHERE asset_id=?`).get(a.id) as Record<string, unknown> | undefined;
      if (transcript) insertRow(db, 'transcripts', { ...transcript, asset_id: newAsset });
      const beatMap = db.prepare(`SELECT * FROM beat_maps WHERE asset_id=?`).get(a.id) as Record<string, unknown> | undefined;
      if (beatMap) insertRow(db, 'beat_maps', { ...beatMap, asset_id: newAsset });
      for (const s of db.prepare(`SELECT * FROM scenes WHERE asset_id=?`).all(a.id) as Record<string, unknown>[]) {
        const frames = (JSON.parse((s.keyframe_paths as string) || '[]') as string[]).map((p) => remapPath(p, srcDir, dstDir));
        insertRow(db, 'scenes', { ...s, id: newId('scn'), asset_id: newAsset, keyframe_paths: JSON.stringify(frames) });
      }
    }
  });
  run();
  return { id: newProjectId, name };
}

/**
 * Delete a project's DB rows and its folder (originals imported from elsewhere are never
 * touched). The caller must refuse the currently open project. The folder is only removed
 * when it is a direct child of the projects root carrying this project's id suffix.
 */
export function deleteProject(
  db: LibDb,
  opts: { projectsRoot: string; projectId: string; onAssetsRemoved?(assetIds: string[], sceneIds: string[]): void },
): void {
  const { projectsRoot, projectId } = opts;
  const exists = db.prepare(`SELECT id FROM projects WHERE id=?`).get(projectId);
  if (!exists) throw new Error(`Project ${projectId} not found`);
  const assetIds = (db.prepare(`SELECT id FROM assets WHERE project_id=?`).all(projectId) as { id: string }[]).map((r) => r.id);
  const sceneIds: string[] = [];
  for (const id of assetIds) {
    for (const s of db.prepare(`SELECT id FROM scenes WHERE asset_id=?`).all(id) as { id: string }[]) sceneIds.push(s.id);
  }
  const dir = findProjectDir(projectsRoot, projectId);

  db.transaction(() => {
    for (const id of assetIds) {
      db.prepare(`DELETE FROM transcripts WHERE asset_id=?`).run(id);
      db.prepare(`DELETE FROM scenes WHERE asset_id=?`).run(id);
      db.prepare(`DELETE FROM beat_maps WHERE asset_id=?`).run(id);
    }
    db.prepare(`DELETE FROM assets WHERE project_id=?`).run(projectId);
    db.prepare(`DELETE FROM jobs WHERE project_id=?`).run(projectId);
    db.prepare(`DELETE FROM exports WHERE project_id=?`).run(projectId);
    db.prepare(`DELETE FROM op_log WHERE project_id=?`).run(projectId);
    db.prepare(`DELETE FROM projects WHERE id=?`).run(projectId);
  })();

  try {
    opts.onAssetsRemoved?.(assetIds, sceneIds);
  } catch {
    /* search index cleanup is best effort */
  }
  if (dir && dirname(resolve(dir)) === resolve(projectsRoot) && basename(dir).endsWith(dirSuffix(projectId))) {
    rmSync(dir, { recursive: true, force: true });
  }
}
