import { mkdtempSync, mkdirSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { DatabaseSync } from 'node:sqlite';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { createEmptyDoc } from '@cutboard/editor-core';
import { newId } from '@cutboard/schema';
import {
  deleteProject,
  dirSuffix,
  duplicateProject,
  findProjectDir,
  listProjects,
  renameProject,
  uniqueProjectName,
  type LibDb,
} from './project-library.ts';

const SCHEMA = `
CREATE TABLE projects (id TEXT PRIMARY KEY, name TEXT NOT NULL, doc TEXT NOT NULL, created_at TEXT NOT NULL, updated_at TEXT NOT NULL);
CREATE TABLE op_log (seq INTEGER PRIMARY KEY AUTOINCREMENT, project_id TEXT NOT NULL, actor TEXT NOT NULL, op TEXT NOT NULL, created_at TEXT NOT NULL);
CREATE TABLE assets (id TEXT PRIMARY KEY, project_id TEXT NOT NULL, kind TEXT NOT NULL, path TEXT NOT NULL, original_name TEXT NOT NULL, proxy_path TEXT, thumb_path TEXT, waveform_path TEXT, created_at TEXT NOT NULL);
CREATE TABLE transcripts (asset_id TEXT PRIMARY KEY, language TEXT NOT NULL DEFAULT 'en', words TEXT NOT NULL DEFAULT '[]');
CREATE TABLE scenes (id TEXT PRIMARY KEY, asset_id TEXT NOT NULL, start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, keyframe_paths TEXT NOT NULL DEFAULT '[]');
CREATE TABLE beat_maps (asset_id TEXT PRIMARY KEY, bpm REAL);
CREATE TABLE jobs (id TEXT PRIMARY KEY, project_id TEXT);
CREATE TABLE exports (id TEXT PRIMARY KEY, project_id TEXT NOT NULL);
`;

/** node:sqlite has no .transaction(); wrap BEGIN/COMMIT so it satisfies LibDb. */
function makeDb(): LibDb {
  const raw = new DatabaseSync(':memory:');
  raw.exec(SCHEMA);
  return {
    prepare: (sql) => {
      const st = raw.prepare(sql);
      return {
        all: (...a) => st.all(...(a as never[])),
        get: (...a) => st.get(...(a as never[])),
        run: (...a) => st.run(...(a as never[])),
      };
    },
    transaction: (fn) => () => {
      raw.exec('BEGIN');
      try {
        const out = fn();
        raw.exec('COMMIT');
        return out;
      } catch (err) {
        raw.exec('ROLLBACK');
        throw err;
      }
    },
  };
}

let root: string;
let db: LibDb;
beforeEach(() => {
  root = mkdtempSync(join(tmpdir(), 'sf-lib-'));
  db = makeDb();
});
afterEach(() => rmSync(root, { recursive: true, force: true }));

function addProject(name: string, opts: { updatedAt?: string; width?: number; height?: number; durationFrames?: number } = {}) {
  const id = newId('prj');
  const doc = createEmptyDoc({ id, name, fps: 30, width: opts.width, height: opts.height });
  if (opts.durationFrames) {
    doc.items.push({ id: newId('itm'), type: 'video', trackId: 't', startFrame: 30, durationFrames: opts.durationFrames, assetId: 'ast_0123456789abcdef' } as never);
  }
  const at = opts.updatedAt ?? new Date().toISOString();
  db.prepare(`INSERT INTO projects (id, name, doc, created_at, updated_at) VALUES (?,?,?,?,?)`).run(id, name, JSON.stringify(doc), at, at);
  const dir = join(root, `${name.toLowerCase().replace(/\W+/g, '-')}${dirSuffix(id)}`);
  mkdirSync(join(dir, 'cache'), { recursive: true });
  mkdirSync(join(dir, 'exports'), { recursive: true });
  return { id, dir };
}

function addAsset(projectId: string, dir: string, fixedId?: string) {
  const id = fixedId ?? newId('ast');
  const thumb = join(dir, 'cache', `${id}.jpg`);
  writeFileSync(thumb, 'x');
  db.prepare(`INSERT INTO assets (id, project_id, kind, path, original_name, thumb_path, created_at) VALUES (?,?,?,?,?,?,?)`).run(
    id, projectId, 'video', 'C:/footage/clip.mp4', 'clip.mp4', thumb, '2026-01-01T00:00:00Z',
  );
  db.prepare(`INSERT INTO transcripts (asset_id, words) VALUES (?, '[]')`).run(id);
  db.prepare(`INSERT INTO scenes (id, asset_id, start_ms, end_ms, keyframe_paths) VALUES (?,?,?,?,?)`).run(
    newId('scn'), id, 0, 1000, JSON.stringify([thumb]),
  );
  db.prepare(`INSERT INTO beat_maps (asset_id, bpm) VALUES (?, 120)`).run(id);
  return { id, thumb };
}

const count = (table: string) => (db.prepare(`SELECT COUNT(*) AS n FROM ${table}`).get() as { n: number }).n;

const copyDirFor = (id: string, name: string) => {
  const dir = join(root, `${name.toLowerCase().replace(/\W+/g, '-')}${dirSuffix(id)}`);
  mkdirSync(dir, { recursive: true });
  return dir;
};

describe('listProjects', () => {
  it('lists every project newest first with size, duration and thumbnail', () => {
    const a = addProject('Old', { updatedAt: '2026-01-01T00:00:00Z' });
    const b = addProject('New', { updatedAt: '2026-02-01T00:00:00Z', width: 1080, height: 1920, durationFrames: 270 });
    const asset = addAsset(b.id, b.dir);
    // far more than the old 50-row limit
    for (let i = 0; i < 60; i++) addProject(`Bulk ${i}`, { updatedAt: '2025-01-01T00:00:00Z' });
    const list = listProjects(db);
    expect(list).toHaveLength(62);
    expect(list[0]).toMatchObject({ id: b.id, width: 1080, height: 1920, fps: 30, durationMs: 10000, thumbPath: asset.thumb });
    expect(list.find((p) => p.id === a.id)).toMatchObject({ durationMs: 0, thumbPath: null, width: 1920, height: 1080 });
  });
});

describe('uniqueProjectName', () => {
  it('numbers a taken name', () => {
    expect(uniqueProjectName(db)).toBe('Untitled video');
    addProject('Untitled video');
    expect(uniqueProjectName(db)).toBe('Untitled video 2');
    addProject('untitled video 2');
    expect(uniqueProjectName(db)).toBe('Untitled video 3');
  });
});

describe('renameProject', () => {
  it('updates the row and the doc, and keeps the folder findable by id', () => {
    const p = addProject('Before');
    renameProject(db, p.id, '  After  ');
    const row = db.prepare(`SELECT name, doc FROM projects WHERE id=?`).get(p.id) as { name: string; doc: string };
    expect(row.name).toBe('After');
    expect(JSON.parse(row.doc).project.name).toBe('After');
    expect(findProjectDir(root, p.id)).toBe(p.dir);
  });
  it('rejects empty and over-long names, and unknown projects', () => {
    const p = addProject('X');
    expect(() => renameProject(db, p.id, '   ')).toThrow();
    expect(() => renameProject(db, p.id, 'a'.repeat(121))).toThrow();
    expect(() => renameProject(db, 'prj_0000000000', 'Y')).toThrow(/not found/);
  });
});

describe('duplicateProject', () => {
  it('copies doc, assets and cache under new ids without touching the original', () => {
    const p = addProject('Reel', { durationFrames: 60 });
    const asset = addAsset(p.id, p.dir, 'ast_0123456789abcdef');
    const copy = duplicateProject(db, { projectsRoot: root, projectId: p.id, dirFor: copyDirFor });
    expect(copy.name).toBe('Reel copy');
    expect(count('projects')).toBe(2);
    expect(count('assets')).toBe(2);
    expect(count('transcripts')).toBe(2);
    expect(count('scenes')).toBe(2);
    expect(count('beat_maps')).toBe(2);

    const copyAsset = db.prepare(`SELECT * FROM assets WHERE project_id=?`).get(copy.id) as { id: string; thumb_path: string; path: string };
    expect(copyAsset.id).not.toBe(asset.id);
    expect(copyAsset.path).toBe('C:/footage/clip.mp4'); // originals are referenced, not copied
    expect(copyAsset.thumb_path.startsWith(findProjectDir(root, copy.id)!)).toBe(true);
    expect(existsSync(copyAsset.thumb_path)).toBe(true);

    const copyDoc = JSON.parse((db.prepare(`SELECT doc FROM projects WHERE id=?`).get(copy.id) as { doc: string }).doc);
    expect(copyDoc.project.id).toBe(copy.id);
    expect(copyDoc.items[0].assetId).toBe(copyAsset.id);
    const origDoc = JSON.parse((db.prepare(`SELECT doc FROM projects WHERE id=?`).get(p.id) as { doc: string }).doc);
    expect(origDoc.items[0].assetId).toBe('ast_0123456789abcdef');
    expect(existsSync(asset.thumb)).toBe(true);
  });

  it('deleting the original leaves the copy intact', () => {
    const p = addProject('Reel');
    addAsset(p.id, p.dir);
    const copy = duplicateProject(db, { projectsRoot: root, projectId: p.id, dirFor: copyDirFor });
    deleteProject(db, { projectsRoot: root, projectId: p.id });
    const copyAsset = db.prepare(`SELECT thumb_path FROM assets WHERE project_id=?`).get(copy.id) as { thumb_path: string };
    expect(existsSync(copyAsset.thumb_path)).toBe(true);
  });
});

describe('deleteProject', () => {
  it('removes every row and the folder, and only that project', () => {
    const keep = addProject('Keep');
    addAsset(keep.id, keep.dir);
    const gone = addProject('Gone');
    const asset = addAsset(gone.id, gone.dir);
    db.prepare(`INSERT INTO jobs (id, project_id) VALUES (?, ?)`).run('job_1', gone.id);
    db.prepare(`INSERT INTO exports (id, project_id) VALUES (?, ?)`).run('exp_1', gone.id);
    db.prepare(`INSERT INTO op_log (project_id, actor, op, created_at) VALUES (?, 'user', '{}', 'now')`).run(gone.id);

    const removed: string[][] = [];
    deleteProject(db, { projectsRoot: root, projectId: gone.id, onAssetsRemoved: (ids) => removed.push(ids) });

    expect(removed).toEqual([[asset.id]]);
    expect(existsSync(gone.dir)).toBe(false);
    expect(existsSync(keep.dir)).toBe(true);
    expect(count('projects')).toBe(1);
    expect(count('assets')).toBe(1);
    expect(count('transcripts')).toBe(1);
    expect(count('scenes')).toBe(1);
    expect(count('beat_maps')).toBe(1);
    expect(count('jobs')).toBe(0);
    expect(count('exports')).toBe(0);
    expect(count('op_log')).toBe(0);
  });

  it('rejects unknown ids and never removes the projects root', () => {
    expect(() => deleteProject(db, { projectsRoot: root, projectId: 'prj_0000000000' })).toThrow(/not found/);
    const p = addProject('Solo');
    deleteProject(db, { projectsRoot: root, projectId: p.id });
    expect(existsSync(root)).toBe(true);
  });
});
