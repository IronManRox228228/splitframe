import { BrowserWindow, ipcMain } from 'electron';
import { join } from 'node:path';
import { getDb } from './db.ts';
import { runFfmpeg, getFfmpeg } from './ffmpeg.ts';
import { projectService } from './project-service.ts';
import { getAsset } from './asset-service.ts';
import { exportMediaPath, selectAudioSources, buildAudioGraph } from './export-plan.ts';
import { TimelineDoc, newId } from '@cutboard/schema';
import { docDurationFrames } from '@cutboard/editor-core';

/**
 * Export pipeline (addendum §3 "Render/export"): a hidden Chromium window renders each
 * frame with the same canvas compositor the preview uses (preview == output), streams
 * raw RGBA frames to ffmpeg over stdin, and ffmpeg muxes hardware-encoded video with a
 * filter_complex audio mix. Queue, progress, cancel; the exports table is SQLite-backed.
 */

export interface ExportPreset {
  name: string;
  width: number;
  height: number;
  format: 'mp4' | 'webm';
  videoBitrateK: number;
}

export const EXPORT_PRESETS: ExportPreset[] = [
  { name: 'TikTok / Reels / Shorts', width: 1080, height: 1920, format: 'mp4', videoBitrateK: 12000 },
  { name: 'YouTube 1080p', width: 1920, height: 1080, format: 'mp4', videoBitrateK: 12000 },
  { name: 'Square 1080', width: 1080, height: 1080, format: 'mp4', videoBitrateK: 10000 },
  { name: '1440p', width: 2560, height: 1440, format: 'mp4', videoBitrateK: 20000 },
  { name: 'WebM 1080p', width: 1920, height: 1080, format: 'webm', videoBitrateK: 10000 },
];

export interface ExportRow {
  id: string;
  projectId: string;
  preset: ExportPreset;
  status: 'queued' | 'rendering' | 'encoding' | 'done' | 'failed' | 'cancelled';
  progress: number;
  outputPath: string | null;
  error: string | null;
  createdAt: string;
  updatedAt: string;
}

type ExportListener = (e: ExportRow) => void;
const exportListeners = new Set<ExportListener>();
export function onExportEvent(listener: ExportListener): () => void {
  exportListeners.add(listener);
  return () => exportListeners.delete(listener);
}

function emit(row: ExportRow): void {
  for (const l of exportListeners) l(row);
}

class ExportService {
  private active = new Map<
    string,
    { controller: AbortController; window: BrowserWindow | null; totalFrames: number; doneFrames: number; stdin: NodeJS.WritableStream | null }
  >();

  async start(presetName: string): Promise<ExportRow> {
    if (!projectService.isOpen) throw new Error('No project open');
    const preset = EXPORT_PRESETS.find((p) => p.name === presetName) ?? EXPORT_PRESETS[0]!;
    const projectId = projectService.projectId;
    const doc = projectService.doc;
    const totalFrames = docDurationFrames(doc);
    if (totalFrames === 0) throw new Error('Timeline is empty — add clips before exporting.');

    const db = getDb();
    const id = newId('exp');
    const now = new Date().toISOString();
    const outputPath = join(projectService.dir, 'exports', `${slug(doc.project.name)}-${id.slice(4, 12)}.${preset.format}`);
    db.prepare(
      `INSERT INTO exports (id, project_id, preset, status, progress, output_path, created_at, updated_at)
       VALUES (?, ?, ?, 'queued', 0, ?, ?, ?)`,
    ).run(id, projectId, JSON.stringify(preset), outputPath, now, now);
    const row = this.get(id)!;
    emit(row);

    const controller = new AbortController();
    const state = { controller, window: null as BrowserWindow | null, totalFrames, doneFrames: 0, stdin: null as NodeJS.WritableStream | null };
    this.active.set(id, state);

    // prepare ffmpeg: video from raw RGBA frames on stdin + audio filter_complex
    const ffmpegReady = this.spawnEncoder(id, doc, preset, outputPath, controller).catch((err) => {
      this.fail(id, err instanceof Error ? err.message : String(err));
    });

    // hidden render window: uses the same compositor code path as the preview
    const win = new BrowserWindow({
      show: false,
      width: preset.width,
      height: preset.height,
      webPreferences: {
        preload: join(__dirname, '../preload/index.cjs'),
        backgroundThrottling: false,
      },
    });
    state.window = win;
    const query = {
      exportId: id,
      width: String(preset.width),
      height: String(preset.height),
      fps: String(doc.project.fps),
      ...(process.env['CUTBOARD_EXPORT_MAX_FRAMES'] ? { maxFrames: process.env['CUTBOARD_EXPORT_MAX_FRAMES'] } : {}),
    };
    if (process.env['ELECTRON_RENDERER_URL']) {
      // dev: the renderer is served by the vite dev server, not from disk
      const u = new URL(process.env['ELECTRON_RENDERER_URL']);
      u.pathname = '/export.html';
      for (const [k, v] of Object.entries(query)) u.searchParams.set(k, v);
      void win.loadURL(u.href);
    } else {
      void win.loadFile(join(__dirname, '../renderer/export.html'), { query });
    }
    win.webContents.on('did-fail-load', (_e, code, desc) => {
      this.fail(id, `Export window failed to load (${code}): ${desc}`);
    });
    win.webContents.on('console-message', (_e, _level, message) => {
      if (message.startsWith('[export]')) process.stderr.write(`[export-window] ${message}\n`);
    });
    win.on('closed', () => {
      if (this.active.has(id)) this.fail(id, 'Render window closed unexpectedly');
    });
    void ffmpegReady;
    return row;
  }

  /** Called from IPC by the hidden window's export script. */
  handleFrame(exportId: string, index: number, buffer: ArrayBuffer, width: number, height: number): void {
    const state = this.active.get(exportId);
    if (!state || !state.stdin) return;
    // RGBA straight from getImageData; ffmpeg reads rawvideo rgba
    void width;
    void height;
    state.stdin.write(Buffer.from(buffer));
    state.doneFrames = index + 1;
    this.update(exportId, { status: 'rendering', progress: 0.9 * ((index + 1) / state.totalFrames) });
  }

  handleFramesDone(exportId: string): void {
    const state = this.active.get(exportId);
    if (!state) return;
    state.stdin?.end();
    this.update(exportId, { status: 'encoding', progress: 0.92 });
  }

  handleWindowError(exportId: string, message: string): void {
    this.fail(exportId, message);
  }

  async cancel(exportId: string): Promise<void> {
    const state = this.active.get(exportId);
    if (!state) return;
    state.controller.abort();
    state.window?.destroy();
    this.active.delete(exportId);
    this.update(exportId, { status: 'cancelled' });
  }

  list(projectId?: string): ExportRow[] {
    const db = getDb();
    const rows = (
      projectId
        ? db.prepare(`SELECT * FROM exports WHERE project_id=? ORDER BY created_at DESC`).all(projectId)
        : db.prepare(`SELECT * FROM exports ORDER BY created_at DESC LIMIT 100`).all()
    ) as Record<string, unknown>[];
    return rows.map((r) => this.rowToExport(r));
  }

  get(id: string): ExportRow | null {
    const db = getDb();
    const row = db.prepare(`SELECT * FROM exports WHERE id=?`).get(id) as Record<string, unknown> | undefined;
    return row ? this.rowToExport(row) : null;
  }

  private rowToExport(r: Record<string, unknown>): ExportRow {
    return {
      id: r.id as string,
      projectId: r.project_id as string,
      preset: JSON.parse(r.preset as string),
      status: r.status as ExportRow['status'],
      progress: r.progress as number,
      outputPath: (r.output_path as string) ?? null,
      error: (r.error as string) ?? null,
      createdAt: r.created_at as string,
      updatedAt: r.updated_at as string,
    };
  }

  private update(id: string, patch: Partial<Pick<ExportRow, 'status' | 'progress' | 'outputPath' | 'error'>>): void {
    const db = getDb();
    const sets = ['updated_at=?'];
    const vals: unknown[] = [new Date().toISOString()];
    if (patch.status !== undefined) (sets.push('status=?'), vals.push(patch.status));
    if (patch.progress !== undefined) (sets.push('progress=?'), vals.push(patch.progress));
    if (patch.outputPath !== undefined) (sets.push('output_path=?'), vals.push(patch.outputPath));
    if (patch.error !== undefined) (sets.push('error=?'), vals.push(patch.error));
    vals.push(id);
    db.prepare(`UPDATE exports SET ${sets.join(', ')} WHERE id=?`).run(...vals);
    const row = this.get(id);
    if (row) emit(row);
  }

  private fail(id: string, message: string): void {
    const state = this.active.get(id);
    state?.window?.destroy();
    state?.controller.abort();
    this.active.delete(id);
    this.update(id, { status: 'failed', error: message });
  }

  private async spawnEncoder(
    exportId: string,
    doc: TimelineDoc,
    preset: ExportPreset,
    outputPath: string,
    controller: AbortController,
  ): Promise<void> {
    const { h264Encoder } = getFfmpeg();
    const fps = doc.project.fps;
    const args: string[] = [
      '-f', 'rawvideo', '-pix_fmt', 'rgba',
      '-s', `${preset.width}x${preset.height}`,
      '-r', String(fps),
      '-i', 'pipe:0',
    ];

    // audio mix: one input per audible item whose file actually has an audio stream
    const audio = buildAudioGraph(selectAudioSources(doc, getAsset), fps);
    if (audio) for (const path of audio.inputs) args.push('-i', path);

    if (preset.format === 'webm') {
      args.push('-c:v', 'libvpx-vp9', '-b:v', `${preset.videoBitrateK}k`, '-row-mt', '1');
    } else {
      const bitrateArgs = h264Encoder.startsWith('libx') ? ['-crf', '20', '-preset', 'medium'] : ['-b:v', `${preset.videoBitrateK}k`];
      args.push('-c:v', h264Encoder, ...bitrateArgs);
    }
    args.push('-pix_fmt', 'yuv420p');
    if (audio) {
      args.push('-filter_complex', audio.filterComplex, '-map', audio.outLabel);
    } else {
      args.push('-an');
    }
    // bound the output to the timeline length; -shortest would cut the video to the audio
    // whenever the audio ends first
    const totalSec = (this.active.get(exportId)?.totalFrames ?? 0) / fps;
    args.push('-map', '0:v', ...(totalSec > 0 ? ['-t', totalSec.toFixed(3)] : []), outputPath);

    const result = await runFfmpeg(args, {
      signal: controller.signal,
      onStdin: (stdin) => {
        const state = this.active.get(exportId);
        if (state) state.stdin = stdin;
      },
      onStderr: (text) => {
        const m = /out_time_ms=(\d+)/.exec(text);
        if (m) {
          const totalUs = (this.active.get(exportId)?.totalFrames ?? 1) / fps * 1_000_000;
          const enc = Number(m[1]); // microseconds
          this.update(exportId, { status: 'encoding', progress: 0.92 + 0.08 * Math.min(1, enc / Math.max(1, totalUs)) });
        }
      },
    });
    if (result.code !== 0 && !controller.signal.aborted) {
      throw new Error(`ffmpeg exited with ${result.code}: ${result.stderr.split('\n').slice(-4).join(' | ')}`);
    }
    if (!controller.signal.aborted) {
      // grab the state first: the render window must be destroyed or it outlives the export
      // and keeps the app from quitting once the main window is closed
      const state = this.active.get(exportId);
      this.active.delete(exportId);
      state?.window?.destroy();
      this.update(exportId, { status: 'done', progress: 1, outputPath });
    }
  }
}

function slug(name: string): string {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'export';
}

export const exportService = new ExportService();

/**
 * captureFrame (main prompt §6): render one timeline frame headlessly with the same
 * compositor and return a PNG buffer. Used by tools/agents — "what the agent sees
 * equals the export".
 */
const stillWaiters = new Map<string, { resolve: (buf: Buffer) => void; reject: (err: Error) => void }>();

export async function renderStill(frame: number, width?: number, height?: number): Promise<Buffer> {
  if (!projectService.isOpen) throw new Error('No project open');
  const doc = projectService.doc;
  const w = width ?? doc.project.width;
  const h = height ?? doc.project.height;
  const id = newId('exp');
  const stillId = `still-${id}`;

  const win = new BrowserWindow({
    show: false,
    width: w,
    height: h,
    webPreferences: {
      preload: join(__dirname, '../preload/index.cjs'),
      backgroundThrottling: false,
    },
  });
  const query = new URLSearchParams({
    exportId: stillId,
    width: String(w),
    height: String(h),
    fps: String(doc.project.fps),
    stillFrame: String(Math.max(0, Math.round(frame))),
  });
  const loadPromise = (async () => {
    if (process.env['ELECTRON_RENDERER_URL']) {
      const u = new URL(process.env['ELECTRON_RENDERER_URL']);
      u.pathname = '/export.html';
      u.search = query.toString();
      await win.loadURL(u.href);
    } else {
      await win.loadFile(join(__dirname, '../renderer/export.html'), { search: query.toString() });
    }
  })();

  const timeout = setTimeout(() => {
    const waiter = stillWaiters.get(stillId);
    if (waiter) {
      stillWaiters.delete(stillId);
      waiter.reject(new Error('captureFrame timed out'));
      win.destroy();
    }
  }, 15000);

  try {
    const png = await new Promise<Buffer>((resolve, reject) => {
      stillWaiters.set(stillId, { resolve, reject });
      void loadPromise.catch(reject);
    });
    return png;
  } finally {
    clearTimeout(timeout);
    stillWaiters.delete(stillId);
    win.destroy();
  }
}

/** IPC surface for the hidden export/still windows (registered at app boot). */
export function registerExportWindowIpc(): void {
  ipcMain.handle('export:bundle', (_e, exportId: string) => {
    if (!projectService.isOpen) throw new Error('No project open');
    const doc = projectService.doc;
    const mediaUrls = new Map<string, string>();
    for (const item of doc.items) {
      if (item.assetId && !mediaUrls.has(item.assetId)) {
        const asset = getAsset(item.assetId);
        if (asset) mediaUrls.set(item.assetId, exportMediaPath(asset));
      }
    }
    return { doc, mediaUrls: Object.fromEntries(mediaUrls) };
  });
  ipcMain.on('export:window:frame', (_e, exportId: string, index: number, buffer: ArrayBuffer, width: number, height: number) => {
    if (exportId.startsWith('still-')) {
      const waiter = stillWaiters.get(exportId);
      if (waiter) waiter.resolve(Buffer.from(buffer));
      void index;
      void width;
      void height;
      return;
    }
    exportService.handleFrame(exportId, index, buffer, width, height);
  });
  ipcMain.on('export:window:done', (_e, exportId: string) => {
    if (exportId.startsWith('still-')) return;
    exportService.handleFramesDone(exportId);
  });
  ipcMain.on('export:window:error', (_e, exportId: string, message: string) => {
    if (exportId.startsWith('still-')) {
      const waiter = stillWaiters.get(exportId);
      if (waiter) waiter.reject(new Error(message));
      return;
    }
    exportService.handleWindowError(exportId, message);
  });
}
