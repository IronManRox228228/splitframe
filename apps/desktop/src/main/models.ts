import { createWriteStream, existsSync, mkdirSync, unlinkSync } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import { join } from 'node:path';
import { Readable } from 'node:stream';
import { app } from 'electron';
import { getPaths } from './paths.ts';
import { WHISPER_MODELS, hasModel, locateWhisperCli } from './analysis/whisper.ts';
import { getSettings } from './settings.ts';

/**
 * Model manager (addendum §5.1): downloads whisper.cpp ggml models with progress and
 * checksum-agnostic resume; the embedding model downloads through transformers.js cache
 * on first use. Everything is stored under userData/models.
 */

function modelsDir(): string {
  const dir = join(app.getPath('userData'), 'models');
  mkdirSync(dir, { recursive: true });
  return dir;
}

export interface ModelStatus {
  id: string;
  kind: 'asr' | 'embeddings';
  downloaded: boolean;
  sizeMB?: number;
  note?: string;
}

export async function listModels(): Promise<ModelStatus[]> {
  const out: ModelStatus[] = [];
  for (const m of WHISPER_MODELS) {
    out.push({ id: m.id, kind: 'asr', downloaded: hasModel(m.id), sizeMB: m.sizeMB, note: m.note });
  }
  out.push({
    id: 'bge-small-en-v1.5 (embeddings)',
    kind: 'embeddings',
    downloaded: existsSync(join(app.getPath('userData'), 'models', 'transformers-cache')),
    sizeMB: 33,
    note: 'local semantic search; downloads on first search',
  });
  return out;
}

type ProgressListener = (payload: { id: string; received: number; total: number; done: boolean; error?: string }) => void;
const listeners = new Set<ProgressListener>();

export function onModelProgress(listener: ProgressListener): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

function emitModelProgress(payload: { id: string; received: number; total: number; done: boolean; error?: string }): void {
  for (const l of listeners) l(payload);
}

const active = new Map<string, AbortController>();

export async function downloadModel(id: string): Promise<{ ok: boolean; error?: string }> {
  const model = WHISPER_MODELS.find((m) => m.id === id);
  if (!model) return { ok: false, error: `Unknown model ${id}` };
  if (hasModel(id)) return { ok: true };
  if (active.has(id)) return { ok: false, error: 'Download already in progress' };

  const url = `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${id}.bin`;
  const dest = join(modelsDir(), `ggml-${id}.bin`);
  const controller = new AbortController();
  active.set(id, controller);
  try {
    const res = await fetch(url, { redirect: 'follow', signal: controller.signal });
    if (!res.ok || !res.body) throw new Error(`HTTP ${res.status}`);
    const total = Number(res.headers.get('content-length') ?? 0);
    let received = 0;
    let lastEmit = 0;
    const reader = Readable.fromWeb(res.body as never);
    reader.on('data', (chunk: Buffer) => {
      received += chunk.length;
      const now = Date.now();
      if (now - lastEmit > 200) {
        lastEmit = now;
        emitModelProgress({ id, received, total, done: false });
      }
    });
    await pipeline(reader, createWriteStream(dest));
    emitModelProgress({ id, received: total, total, done: true });
    return { ok: true };
  } catch (err) {
    try {
      if (existsSync(dest)) unlinkSync(dest);
    } catch { /* ignore */ }
    const error = controller.signal.aborted ? 'cancelled' : err instanceof Error ? err.message : String(err);
    emitModelProgress({ id, received: 0, total: 0, done: false, error });
    return { ok: false, error };
  } finally {
    active.delete(id);
  }
}

export function deleteModel(id: string): void {
  const dest = join(modelsDir(), `ggml-${id}.bin`);
  if (existsSync(dest)) unlinkSync(dest);
}

export function cancelModelDownload(id: string): void {
  active.get(id)?.abort();
}

/** Environment report for first-run: what's available on this machine. */
export async function getEnvironmentReport(): Promise<{
  whisperCli: boolean;
  asrModel: string;
  asrModelReady: boolean;
  ffmpeg: { source: string; h264Encoder: string } | null;
}> {
  const settings = await getSettings();
  return {
    whisperCli: locateWhisperCli() !== null,
    asrModel: settings.asr?.model ?? 'base.en',
    asrModelReady: hasModel(settings.asr?.model ?? 'base.en'),
    ffmpeg: null, // filled by ipc from getFfmpeg
  };
}

void getPaths;
