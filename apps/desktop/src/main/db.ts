import Database from 'better-sqlite3';
import { getPaths } from './paths.ts';

let db: Database.Database | null = null;

const SCHEMA = `
CREATE TABLE IF NOT EXISTS projects (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  doc TEXT NOT NULL,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS op_log (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  project_id TEXT NOT NULL,
  actor TEXT NOT NULL,
  op TEXT NOT NULL,
  created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS op_log_project ON op_log (project_id, seq);

CREATE TABLE IF NOT EXISTS assets (
  id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  path TEXT NOT NULL,
  original_name TEXT NOT NULL,
  proxy_path TEXT,
  thumb_path TEXT,
  waveform_path TEXT,
  status TEXT NOT NULL DEFAULT 'importing',
  stage TEXT,
  duration_ms INTEGER NOT NULL DEFAULT 0,
  width INTEGER NOT NULL DEFAULT 0,
  height INTEGER NOT NULL DEFAULT 0,
  fps REAL,
  has_audio INTEGER NOT NULL DEFAULT 0,
  has_speech INTEGER NOT NULL DEFAULT 0,
  size_bytes INTEGER NOT NULL DEFAULT 0,
  mtime_ms INTEGER NOT NULL DEFAULT 0,
  hash TEXT,
  metadata TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  error TEXT
);
CREATE INDEX IF NOT EXISTS assets_project ON assets (project_id);

CREATE TABLE IF NOT EXISTS transcripts (
  asset_id TEXT PRIMARY KEY,
  language TEXT NOT NULL DEFAULT 'en',
  words TEXT NOT NULL DEFAULT '[]'
);

CREATE TABLE IF NOT EXISTS scenes (
  id TEXT PRIMARY KEY,
  asset_id TEXT NOT NULL,
  start_ms INTEGER NOT NULL,
  end_ms INTEGER NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  tags TEXT NOT NULL DEFAULT '[]',
  keyframe_paths TEXT NOT NULL DEFAULT '[]'
);
CREATE INDEX IF NOT EXISTS scenes_asset ON scenes (asset_id);

CREATE TABLE IF NOT EXISTS beat_maps (
  asset_id TEXT PRIMARY KEY,
  bpm REAL,
  beats TEXT NOT NULL DEFAULT '[]',
  downbeats TEXT NOT NULL DEFAULT '[]',
  sections TEXT NOT NULL DEFAULT '[]'
);

CREATE TABLE IF NOT EXISTS silence_maps (
  asset_id TEXT PRIMARY KEY,
  version INTEGER NOT NULL,
  silences TEXT NOT NULL DEFAULT '[]'
);

CREATE TABLE IF NOT EXISTS footage_notes (
  asset_id TEXT PRIMARY KEY,
  version INTEGER NOT NULL,
  notes TEXT NOT NULL,
  computed_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS jobs (
  id TEXT PRIMARY KEY,
  project_id TEXT,
  type TEXT NOT NULL,
  payload TEXT NOT NULL DEFAULT '{}',
  status TEXT NOT NULL DEFAULT 'pending',
  progress REAL NOT NULL DEFAULT 0,
  stage TEXT,
  error TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS jobs_status ON jobs (status);

CREATE TABLE IF NOT EXISTS exports (
  id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL,
  preset TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'queued',
  progress REAL NOT NULL DEFAULT 0,
  output_path TEXT,
  error TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;
`;

export function getDb(): Database.Database {
  if (db) return db;
  const { dbPath } = getPaths();
  db = new Database(dbPath);
  db.exec(SCHEMA);
  invalidateStaleAnalysis(db);
  return db;
}

/**
 * Bump when a detector's output changes, and drop its cached rows here; they are recomputed
 * on demand. 1: beat detector rewrite (old maps were 1 BPM off with a drifting grid).
 */
const ANALYSIS_CACHE_VERSION = 1;

function invalidateStaleAnalysis(db: Database.Database): void {
  const version = db.pragma('user_version', { simple: true }) as number;
  if (version >= ANALYSIS_CACHE_VERSION) return;
  if (version < 1) db.exec(`DELETE FROM beat_maps`);
  db.pragma(`user_version = ${ANALYSIS_CACHE_VERSION}`);
}

export function closeDb(): void {
  db?.close();
  db = null;
}
