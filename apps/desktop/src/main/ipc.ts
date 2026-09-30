import { BrowserWindow, dialog, ipcMain, shell } from 'electron';
import { z } from 'zod';
import { Op, opSchema, Actor } from '@cutboard/schema';
import { projectService } from './project-service.ts';
import { checkAssetAvailability, getAsset, importFiles, relinkAsset, removeAsset, onAssetEvent } from './asset-service.ts';
import { exportService, EXPORT_PRESETS, registerExportWindowIpc, onExportEvent } from './export-service.ts';
import { jobs, JobRow, onJobEvent } from './jobs.ts';
import { getFfmpeg, FfmpegInfo } from './ffmpeg.ts';
import { getPaths } from './paths.ts';

/**
 * IPC is the security boundary (addendum §5.4): the renderer is untrusted, every
 * handler validates its inputs with zod, and only the allowlisted channels below exist.
 */

const opsInput = z.object({
  ops: z.array(opSchema).min(1).max(500),
  groupLabel: z.string().max(120).optional(),
});

const actorFor = (event: Electron.IpcMainInvokeEvent): Actor => {
  const win = BrowserWindow.fromWebContents(event.sender);
  const isExportWindow = win?.webContents.getURL().includes('export.html') ?? false;
  return isExportWindow ? 'user' : 'user';
};

export function registerIpc(broadcast: (channel: string, payload: unknown) => void): void {
  // ---------- app ----------
  ipcMain.handle('app:info', () => {
    let ffmpeg: FfmpegInfo | null = null;
    try {
      ffmpeg = getFfmpeg();
    } catch {
      ffmpeg = null;
    }
    return {
      version: '0.1.0',
      platform: process.platform,
      projectsRoot: getPaths().projectsRoot,
      ffmpeg,
    };
  });

  ipcMain.handle('dialog:pickMedia', async (e) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const res = await dialog.showOpenDialog(win!, {
      title: 'Import media',
      properties: ['openFile', 'multiSelections'],
      filters: [
        { name: 'Media', extensions: ['mp4', 'mov', 'm4v', 'webm', 'mkv', 'avi', 'gif', 'mp3', 'wav', 'm4a', 'aac', 'ogg', 'flac', 'aiff', 'png', 'jpg', 'jpeg', 'webp', 'bmp'] },
      ],
    });
    if (res.canceled || res.filePaths.length === 0) return [];
    return importFiles(res.filePaths);
  });

  ipcMain.handle('dialog:pickFile', async (e) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const res = await dialog.showOpenDialog(win!, {
      title: 'Choose file',
      properties: ['openFile'],
    });
    if (res.canceled || res.filePaths.length === 0) return null;
    return res.filePaths[0]!;
  });

  // ---------- projects ----------
  ipcMain.handle('projects:listRecent', () => projectService.listRecent());
  ipcMain.handle(
    'projects:create',
    (_e, input: { name?: string; fps?: number; width?: number; height?: number }) => {
      const parsed = z
        .object({
          name: z.string().min(1).max(120).optional(),
          fps: z.number().int().min(1).max(120).optional(),
          width: z.number().int().min(16).max(7680).optional(),
          height: z.number().int().min(16).max(4320).optional(),
        })
        .parse(input ?? {});
      return projectService.create(parsed.name ?? 'Untitled project', parsed);
    },
  );
  ipcMain.handle('projects:open', (_e, projectId: string) => {
    z.string().regex(/^prj_[0-9a-f]{10,}$/).parse(projectId);
    return projectService.open(projectId);
  });
  ipcMain.handle('projects:close', () => {
    projectService.close();
    broadcast('event', { type: 'doc:closed' });
    return true;
  });
  ipcMain.handle('projects:reveal', () => {
    if (!projectService.isOpen) return false;
    void shell.openPath(projectService.dir);
    return true;
  });

  // ---------- ops / history ----------
  ipcMain.handle(
    'ops:apply',
    (e, input: { ops: Op[]; groupLabel?: string }) => {
      const parsed = opsInput.parse(input);
      const result = projectService.apply(parsed.ops, actorFor(e), parsed.groupLabel);
      broadcast('event', {
        type: 'doc:changed',
        payload: {
          doc: projectService.doc,
          seq: result.seq,
          actor: 'user',
          label: parsed.groupLabel ?? null,
        },
      });
      return { ok: true, seq: result.seq, inverses: result.inverses, doc: projectService.doc };
    },
  );
  ipcMain.handle('history:undo', () => {
    const res = projectService.undo();
    if (res) broadcast('event', { type: 'doc:changed', payload: { doc: projectService.doc, label: `Undo${res.label ? `: ${res.label}` : ''}` } });
    return res ? { ok: true, doc: projectService.doc } : { ok: false };
  });
  ipcMain.handle('history:redo', () => {
    const res = projectService.redo();
    if (res) broadcast('event', { type: 'doc:changed', payload: { doc: projectService.doc, label: `Redo${res.label ? `: ${res.label}` : ''}` } });
    return res ? { ok: true, doc: projectService.doc } : { ok: false };
  });
  ipcMain.handle('history:labels', () => projectService.historyLabels);

  // ---------- assets ----------
  ipcMain.handle('assets:get', (_e, assetId: string) => getAsset(assetId));
  ipcMain.handle('assets:checkAvailability', (_e, assetId: string) => checkAssetAvailability(assetId));
  ipcMain.handle(
    'assets:relink',
    (_e, input: { assetId: string; newPath: string }) => {
      const parsed = z.object({ assetId: z.string(), newPath: z.string().min(1) }).parse(input);
      return relinkAsset(parsed.assetId, parsed.newPath);
    },
  );
  ipcMain.handle(
    'assets:remove',
    (_e, assetId: string) => {
      z.string().parse(assetId);
      removeAsset(assetId);
      return true;
    },
  );

  // ---------- jobs ----------
  ipcMain.handle('jobs:list', (_e, projectId?: string) => jobs.list(projectId));
  ipcMain.handle('jobs:cancel', (_e, jobId: string) => {
    jobs.cancel(jobId);
    return true;
  });

  // ---------- exports ----------
  ipcMain.handle('exports:presets', () => EXPORT_PRESETS);
  ipcMain.handle('exports:list', (_e, projectId?: string) => exportService.list(projectId));
  ipcMain.handle('exports:start', (_e, presetName: string) => {
    z.string().min(1).parse(presetName);
    return exportService.start(presetName);
  });
  ipcMain.handle('exports:cancel', (_e, exportId: string) => exportService.cancel(exportId));

  // hidden export window channels
  registerExportWindowIpc();
}

/** Forward main-process events (jobs, assets, exports) to all windows. */
export function wireEvents(broadcast: (channel: string, payload: unknown) => void): void {
  onJobEvent((job: JobRow) => broadcast('event', { type: 'job', payload: job }));
  onAssetEvent((asset) => broadcast('event', { type: 'asset', payload: asset }));
  onExportEvent((row) => broadcast('event', { type: 'export', payload: row }));
}
