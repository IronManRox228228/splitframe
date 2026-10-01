import { getDb } from '../db.ts';
import * as sqliteVec from 'sqlite-vec';
import type { Transcript } from '@cutboard/schema';

/**
 * Search stack (main prompt §5, addendum §3): FTS5 over transcript words, sqlite-vec
 * over scene-description embeddings, hybrid rank fusion for searchFootage. Vector search
 * activates when the embedding model is present; FTS always works.
 */

export function initSearchSchema(): void {
  const db = getDb();
  db.exec(`
    CREATE TABLE IF NOT EXISTS words (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      asset_id TEXT NOT NULL,
      start_ms INTEGER NOT NULL,
      end_ms INTEGER NOT NULL,
      text TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS words_asset ON words (asset_id, start_ms);
    CREATE VIRTUAL TABLE IF NOT EXISTS words_fts USING fts5(text, content='words', content_rowid='id', tokenize='unicode61');
    CREATE TRIGGER IF NOT EXISTS words_ai AFTER INSERT ON words BEGIN
      INSERT INTO words_fts(rowid, text) VALUES (new.id, new.text);
    END;
    CREATE TRIGGER IF NOT EXISTS words_ad AFTER DELETE ON words BEGIN
      INSERT INTO words_fts(words_fts, rowid, text) VALUES ('delete', old.id, old.text);
    END;
  `);
  try {
    sqliteVec.load(db as never);
    db.exec(`
      CREATE VIRTUAL TABLE IF NOT EXISTS scene_vec USING vec0(
        scene_id TEXT PRIMARY KEY,
        embedding float[384]
      );
    `);
    vectorSearchEnabled = true;
  } catch {
    // sqlite-vec unavailable: FTS-only search (documented degradation)
    vectorSearchEnabled = false;
  }
}

let vectorSearchEnabled = false;

export function isVectorSearchEnabled(): boolean {
  return vectorSearchEnabled;
}

export function indexTranscriptWords(assetId: string, words: Transcript['words']): void {
  const db = getDb();
  const tx = db.transaction(() => {
    db.prepare(`DELETE FROM words WHERE asset_id=?`).run(assetId);
    const ins = db.prepare(`INSERT INTO words (asset_id, start_ms, end_ms, text) VALUES (?, ?, ?, ?)`);
    for (const w of words) ins.run(assetId, w.startMs, w.endMs, w.w);
  });
  tx();
}

export interface WordHit {
  assetId: string;
  text: string;
  startMs: number;
  endMs: number;
}

/** Full-text word search → group consecutive hits into phrase ranges. */
export function searchWords(query: string, limit = 60): WordHit[] {
  const db = getDb();
  // sanitize FTS query: quote each term to avoid syntax errors on punctuation
  const terms = query.trim().split(/\s+/).filter(Boolean).map((t) => `"${t.replaceAll('"', '')}"`);
  if (terms.length === 0) return [];
  const rows = db
    .prepare(
      `SELECT w.asset_id, w.start_ms, w.end_ms, w.text
       FROM words_fts f JOIN words w ON w.id = f.rowid
       WHERE words_fts MATCH ?
       ORDER BY rank LIMIT ?`,
    )
    .all(terms.join(' '), limit) as { asset_id: string; start_ms: number; end_ms: number; text: string }[];
  return rows.map((r) => ({ assetId: r.asset_id, text: r.text, startMs: r.start_ms, endMs: r.end_ms }));
}

/** Group word hits into nearby phrase matches (gap < 1200ms) per asset. */
export function groupWordHits(hits: WordHit[]): { assetId: string; startMs: number; endMs: number; text: string }[] {
  const byAsset = new Map<string, WordHit[]>();
  for (const hit of hits) {
    const list = byAsset.get(hit.assetId) ?? [];
    list.push(hit);
    byAsset.set(hit.assetId, list);
  }
  const groups: { assetId: string; startMs: number; endMs: number; text: string }[] = [];
  for (const [assetId, list] of byAsset) {
    list.sort((a, b) => a.startMs - b.startMs);
    let current: WordHit[] = [];
    const flush = () => {
      if (current.length === 0) return;
      groups.push({
        assetId,
        startMs: current[0]!.startMs,
        endMs: current[current.length - 1]!.endMs,
        text: current.map((w) => w.text).join(' '),
      });
      current = [];
    };
    for (const hit of list) {
      if (current.length > 0 && hit.startMs - current[current.length - 1]!.endMs > 1200) flush();
      current.push(hit);
    }
    flush();
  }
  return groups.slice(0, 20);
}

// ---------- embeddings ----------

type EmbedFn = (texts: string[]) => Promise<number[][]>;
let embedFn: EmbedFn | null = null;
let embedLoadAttempted = false;

/**
 * Lazily load the bge-small embedding pipeline via transformers.js (local ONNX, no data
 * leaves the machine). Returns null when the model isn't downloaded/available.
 */
async function getEmbedFn(): Promise<EmbedFn | null> {
  if (embedFn) return embedFn;
  if (embedLoadAttempted) return null;
  embedLoadAttempted = true;
  try {
    const { app } = await import('electron');
    const { join } = await import('node:path');
    const cacheDir = join(app.getPath('userData'), 'models', 'transformers-cache');
    const mod = (await import('@huggingface/transformers')) as unknown as {
      pipeline: (task: string, model: string, opts: Record<string, unknown>) => Promise<unknown>;
      env: { cacheDir?: string; allowRemoteModels: boolean };
    };
    mod.env.cacheDir = cacheDir;
    const extractor = (await mod.pipeline('feature-extraction', 'Xenova/bge-small-en-v1.5', {
      quantized: true,
    })) as (input: string[], opts: Record<string, unknown>) => Promise<{ tolist: () => number[][] }>;
    embedFn = async (texts: string[]) => {
      const out = await extractor(texts, { pooling: 'mean', normalize: true });
      return out.tolist();
    };
    return embedFn;
  } catch (err) {
    process.stderr.write(`[embeddings] unavailable: ${err instanceof Error ? err.message : String(err)}\n`);
    return null;
  }
}

export async function embedTexts(texts: string[]): Promise<number[][] | null> {
  const fn = await getEmbedFn();
  if (!fn || texts.length === 0) return null;
  return fn(texts);
}

/** Embed + upsert one scene's description vector. */
export async function indexScene(sceneId: string, description: string): Promise<boolean> {
  if (!vectorSearchEnabled) return false;
  const vectors = await embedTexts([description]);
  if (!vectors) return false;
  const db = getDb();
  db.prepare(`DELETE FROM scene_vec WHERE scene_id=?`).run(sceneId);
  db.prepare(`INSERT INTO scene_vec (scene_id, embedding) VALUES (?, ?)`).run(sceneId, JSON.stringify(vectors[0]));
  return true;
}

/** Semantic scene search: query vector → kNN over scene_vec, joined with scenes. */
export async function searchScenes(
  query: string,
  limit = 12,
): Promise<{ sceneId: string; assetId: string; startMs: number; endMs: number; description: string; distance: number }[]> {
  if (!vectorSearchEnabled) return [];
  const vectors = await embedTexts([query]);
  if (!vectors) return [];
  const db = getDb();
  const rows = db
    .prepare(
      `SELECT v.scene_id, s.asset_id, s.start_ms, s.end_ms, s.description, v.distance
       FROM scene_vec v JOIN scenes s ON s.id = v.scene_id
       WHERE v.embedding MATCH ?
       ORDER BY distance LIMIT ?`,
    )
    .all(JSON.stringify(vectors[0]), limit) as {
    scene_id: string;
    asset_id: string;
    start_ms: number;
    end_ms: number;
    description: string;
    distance: number;
  }[];
  return rows.map((r) => ({
    sceneId: r.scene_id,
    assetId: r.asset_id,
    startMs: r.start_ms,
    endMs: r.end_ms,
    description: r.description,
    distance: r.distance,
  }));
}
