import { stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { basename, join } from 'node:path';
import { getDb } from './db.ts';
import { getPaths, projectDir } from './paths.ts';
import { ffprobe, makeProxy, makeThumbnail, makeWaveform } from './ffmpeg.ts';
import { jobs, JobRow } from './jobs.ts';
import { projectService } from './project-service.ts';
import { transcribe } from './analysis/whisper.ts';
import { detectScenes, extractKeyframe, keyframePath } from './analysis/scenes.ts';
import { describeKeyframe, getVlmConfig } from './analysis/vlm.ts';
import { indexTranscriptWords, indexScene } from './analysis/search.ts';
import { getSettings } from './settings.ts';
import { Asset, AssetKind, assetSchema, newId } from '@cutboard/schema';

type AssetListener = (asset: Asset) => void;
const assetListeners = new Set<AssetListener>();

export function onAssetEvent(listener: AssetListener): () => void {
  assetListeners.add(listener);
  return () => assetListeners.delete(listener);
}

function rowToAsset(r: Record<string, unknown>): Asset {
  return assetSchema.parse({
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
  });
}

function saveAsset(asset: Asset): void {
  const db = getDb();
  db.prepare(
    `UPDATE assets SET status=?, stage=?, proxy_path=?, thumb_path=?, waveform_path=?,
     duration_ms=?, width=?, height=?, fps=?, has_audio=?, has_speech=?, error=? WHERE id=?`,
  ).run(
    asset.status,
    asset.stage ?? null,
    asset.proxyPath ?? null,
    asset.thumbPath ?? null,
    asset.waveformPath ?? null,
    asset.durationMs,
    asset.width,
    asset.height,
    asset.fps ?? null,
    asset.hasAudio ? 1 : 0,
    asset.hasSpeech ? 1 : 0,
    asset.error ?? null,
    asset.id,
  );
  for (const l of assetListeners) l(asset);
}

function kindFromProbe(probe: { hasVideo: boolean; hasAudio: boolean }): AssetKind {
  if (probe.hasVideo) return 'video';
  if (probe.hasAudio) return 'audio';
  return 'image';
}

/** Supported media extensions (browser-playable + common camera formats via proxies). */
const MEDIA_EXTENSIONS = new Set([
  'mp4', 'mov', 'm4v', 'webm', 'mkv', 'avi', 'gif',
  'mp3', 'wav', 'm4a', 'aac', 'ogg', 'flac', 'aiff',
  'png', 'jpg', 'jpeg', 'webp', 'bmp',
]);

export function isMediaFile(path: string): boolean {
  const ext = path.split('.').pop()?.toLowerCase() ?? '';
  return MEDIA_EXTENSIONS.has(ext);
}

/**
 * Import = reference the original where it is (addendum §2), then generate proxies +
 * thumbnails in the project cache folder. Ingest jobs are queued: proxy → thumb →
 * waveform → analyzed. (ASR/scenes/VLM/embeddings arrive with milestone 2.)
 */
async function importFile(filePath: string): Promise<Asset> {
  if (!projectService.isOpen) throw new Error('Open a project first');
  const projectId = projectService.projectId;

  const st = await stat(filePath);
  const db = getDb();
  const id = newId('ast');
  const now = new Date().toISOString();
  const asset: Asset = assetSchema.parse({
    id,
    projectId,
    kind: 'video',
    path: filePath,
    originalName: basename(filePath),
    status: 'processing',
    stage: 'probe',
    sizeBytes: st.size,
    mtimeMs: Math.round(st.mtimeMs),
    metadata: {},
    createdAt: now,
  });
  db.prepare(
    `INSERT INTO assets (id, project_id, kind, path, original_name, status, stage, size_bytes, mtime_ms, metadata, created_at)
     VALUES (?, ?, ?, ?, ?, 'processing', 'probe', ?, ?, '{}', ?)`,
  ).run(id, projectId, 'video', filePath, asset.originalName, st.size, Math.round(st.mtimeMs), now);

  // probe synchronously (fast) so the kind/dimensions are correct immediately
  try {
    const probe = await ffprobe(filePath);
    asset.kind = kindFromProbe(probe);
    asset.durationMs = probe.durationMs;
    asset.width = probe.width;
    asset.height = probe.height;
    asset.fps = probe.fps;
    asset.hasAudio = probe.hasAudio;
    if (probe.codec) asset.metadata = { ...asset.metadata, codec: probe.codec };
    asset.stage = 'queued';
    saveAsset(asset);
    db.prepare(`UPDATE assets SET kind=?, duration_ms=?, width=?, height=?, fps=?, has_audio=?, metadata=? WHERE id=?`).run(
      asset.kind,
      asset.durationMs,
      asset.width,
      asset.height,
      asset.fps ?? null,
      asset.hasAudio ? 1 : 0,
      JSON.stringify(asset.metadata),
      id,
    );
  } catch (err) {
    asset.status = 'failed';
    asset.error = `Probe failed: ${err instanceof Error ? err.message : String(err)}`;
    saveAsset(asset);
    return asset;
  }

  jobs.enqueue('ingest-asset', projectId, { assetId: id });
  return asset;
}

export async function importFiles(paths: string[]): Promise<Asset[]> {
  const out: Asset[] = [];
  for (const p of paths) {
    if (!isMediaFile(p)) continue;
    out.push(await importFile(p));
  }
  return out;
}

function registerIngestHandler(): void {
  jobs.register('ingest-asset', async (job: JobRow, ctx) => {
    const assetId = String(job.payload.assetId);
    const db = getDb();
    const row = db.prepare(`SELECT * FROM assets WHERE id=?`).get(assetId) as Record<string, unknown> | undefined;
    if (!row) throw new Error(`Asset ${assetId} missing`);
    const asset = rowToAsset(row);
    const dir = projectService.isOpen ? projectService.dir : projectDir(getPaths().projectsRoot, asset.projectId, 'assets');
    const cache = join(dir, 'cache');

    ctx.progress(0.03, 'proxy');
    const proxyPath = join(cache, `${asset.id}-proxy.mp4`);
    let proxyOk = asset.kind !== 'video';
    if (asset.kind === 'video') {
      try {
        await makeProxy(asset.path, proxyPath, {
          durationMs: asset.durationMs,
          signal: ctx.signal,
          onProgress: (done, total) => ctx.progress(0.03 + 0.27 * (done / Math.max(1, total)), 'proxy'),
        });
        proxyOk = existsSync(proxyPath);
      } catch (err) {
        process.stderr.write(`[ingest] proxy failed for ${assetId}: ${err instanceof Error ? err.message : String(err)}\n`);
      }
    }
    if (ctx.signal.aborted) return;
    // a proxy that was never written must not be recorded; the preview falls back to the original
    asset.proxyPath = asset.kind === 'video' && proxyOk ? proxyPath : asset.path;
    saveAsset(asset);

    ctx.progress(0.32, 'thumbnail');
    if (asset.kind === 'video') {
      const thumbPath = join(cache, `${asset.id}-thumb.jpg`);
      try {
        await makeThumbnail(asset.path, thumbPath, Math.min(2, asset.durationMs / 2000 || 0.1));
        if (existsSync(thumbPath)) asset.thumbPath = thumbPath;
      } catch {
        // thumbnails are cosmetic; ignore failures
      }
    } else if (asset.kind === 'image') {
      asset.thumbPath = asset.path;
    }
    saveAsset(asset);

    ctx.progress(0.36, 'waveform');
    if (asset.hasAudio) {
      const wavePath = join(cache, `${asset.id}-wave.png`);
      try {
        await makeWaveform(asset.path, wavePath);
        asset.waveformPath = wavePath;
      } catch {
        // waveform is cosmetic; ignore failures
      }
    }
    saveAsset(asset);
    if (ctx.signal.aborted) return;

    // ---- ASR (word-level transcript) — audio-bearing assets only ----
    if (asset.hasAudio) {
      ctx.progress(0.4, 'transcribe');
      const settings = await getSettings();
      try {
        const { words, language } = await transcribe(asset.path, {
          modelId: settings.asr?.model ?? 'base.en',
          workDir: cache,
          signal: ctx.signal,
          progress: (p) => ctx.progress(0.4 + 0.25 * p, 'transcribe'),
        });
        if (words.length > 0) {
          db.prepare(
            `INSERT OR REPLACE INTO transcripts (asset_id, language, words) VALUES (?, ?, ?)`,
          ).run(assetId, language, JSON.stringify(words));
          indexTranscriptWords(assetId, words);
          // speech-quality hint for take selection
          const speechMs = words.reduce((sum, w) => sum + (w.endMs - w.startMs), 0);
          db.prepare(`UPDATE assets SET has_speech=? WHERE id=?`).run(speechMs > 2000 ? 1 : 0, assetId);
          asset.hasSpeech = speechMs > 2000;
        }
      } catch (err) {
        // ASR unavailable (no model) or failed: degrade gracefully, keep analyzing
        process.stderr.write(`[ingest] ASR skipped for ${assetId}: ${err instanceof Error ? err.message : String(err)}\n`);
      }
    }
    saveAsset(asset);
    if (ctx.signal.aborted) return;

    // ---- scene detection + keyframes (video only) ----
    if (asset.kind === 'video') {
      ctx.progress(0.68, 'scenes');
      let scenes: { startMs: number; endMs: number }[] = [];
      try {
        scenes = await detectScenes(asset.path, asset.durationMs, { signal: ctx.signal });
      } catch {
        scenes = [{ startMs: 0, endMs: asset.durationMs }]; // single scene fallback
      }
      if (scenes.length === 0) scenes = [{ startMs: 0, endMs: asset.durationMs }];
      const { provider } = await getVlmConfig();
      for (let i = 0; i < scenes.length; i++) {
        if (ctx.signal.aborted) return;
        const scene = scenes[i]!;
        const kfPath = keyframePath(cache, assetId, i);
        try {
          await extractKeyframe(asset.path, (scene.startMs + scene.endMs) / 2, kfPath);
        } catch { /* continue without keyframe */ }
        let description = '';
        const tags: string[] = [];
        if (provider !== 'none') {
          try {
            description = await describeKeyframe(kfPath, `Scene ${i + 1}/${scenes.length} of ${asset.originalName}.`);
          } catch (err) {
            process.stderr.write(`[ingest] VLM skipped: ${err instanceof Error ? err.message : String(err)}\n`);
          }
        }
        const sceneId = newId('scn');
        db.prepare(
          `INSERT OR REPLACE INTO scenes (id, asset_id, start_ms, end_ms, description, tags, keyframe_paths) VALUES (?, ?, ?, ?, ?, ?, ?)`,
        ).run(sceneId, assetId, scene.startMs, scene.endMs, description, JSON.stringify(tags), JSON.stringify([kfPath]));
        if (description) await indexScene(sceneId, description);
        ctx.progress(0.68 + 0.24 * ((i + 1) / scenes.length), 'scenes');
      }
    }

    ctx.progress(1, 'analyzed');
    asset.status = 'analyzed';
    asset.stage = undefined;
    saveAsset(asset);
    if (ctx.signal.aborted) return;
  });
}

registerIngestHandler();

export function getAssets(projectId: string): Asset[] {
  const db = getDb();
  const rows = db.prepare(`SELECT * FROM assets WHERE project_id=? ORDER BY created_at`).all(projectId) as Record<string, unknown>[];
  return rows.map(rowToAsset);
}

export function getAsset(assetId: string): Asset | null {
  const db = getDb();
  const row = db.prepare(`SELECT * FROM assets WHERE id=?`).get(assetId) as Record<string, unknown> | undefined;
  return row ? rowToAsset(row) : null;
}

/** Detect originals that moved/went missing (addendum §2 "Relink" precondition). */
export async function checkAssetAvailability(assetId: string): Promise<'ok' | 'missing'> {
  const asset = getAsset(assetId);
  if (!asset) return 'missing';
  try {
    const st = await stat(asset.path);
    if (st.isFile()) {
      if (asset.status === 'missing') {
        saveAsset({ ...asset, status: 'analyzed' });
      }
      return 'ok';
    }
  } catch {
    /* fallthrough */
  }
  const current = getAsset(assetId)!;
  if (current.status !== 'missing') saveAsset({ ...current, status: 'missing' });
  return 'missing';
}

export async function relinkAsset(assetId: string, newPath: string): Promise<Asset> {
  const asset = getAsset(assetId);
  if (!asset) throw new Error(`Asset ${assetId} not found`);
  const st = await stat(newPath);
  if (!st.isFile()) throw new Error('Not a file');
  const updated: Asset = { ...asset, path: newPath, originalName: basename(newPath), status: 'analyzed', error: undefined };
  const db = getDb();
  db.prepare(`UPDATE assets SET path=?, original_name=?, status='analyzed', error=NULL WHERE id=?`).run(newPath, updated.originalName, assetId);
  for (const l of assetListeners) l(updated);
  return updated;
}

export function removeAsset(assetId: string): void {
  const db = getDb();
  db.prepare(`DELETE FROM assets WHERE id=?`).run(assetId);
  db.prepare(`DELETE FROM transcripts WHERE asset_id=?`).run(assetId);
  db.prepare(`DELETE FROM scenes WHERE asset_id=?`).run(assetId);
  db.prepare(`DELETE FROM beat_maps WHERE asset_id=?`).run(assetId);
}

/** Word-level transcripts for every asset in a project (empty until ASR runs). */
export function getTranscriptsForProject(projectId: string): { assetId: string; language: string; words: unknown[] }[] {
  const db = getDb();
  const rows = db
    .prepare(`SELECT t.* FROM transcripts t JOIN assets a ON a.id = t.asset_id WHERE a.project_id=?`)
    .all(projectId) as Record<string, unknown>[];
  return rows.map((r) => ({ assetId: r.asset_id as string, language: (r.language as string) ?? 'en', words: JSON.parse((r.words as string) ?? '[]') }));
}

/** Assets are read by the renderer via cbmedia://media/<encodeURIComponent(path)> */
export function mediaUrlFor(path: string): string {
  return `cbmedia://media/${encodeURIComponent(path)}`;
}
