import { contextBridge, ipcRenderer } from 'electron';

/**
 * Typed, allowlisted bridge. The renderer never touches Node — every capability is an
 * explicit invoke channel validated again in the main process (addendum §5.4).
 */
export interface CutboardApi {
  appInfo(): Promise<{
    version: string;
    platform: string;
    projectsRoot: string;
    ffmpeg: { ffmpegPath: string; ffprobePath: string; source: string; h264Encoder: string; version: string } | null;
  }>;
  pickMediaFiles(): Promise<unknown[]>;
  pickFilePath(): Promise<string | null>;
  listRecentProjects(): Promise<{ id: string; name: string; updatedAt: string }[]>;
  createProject(input?: { name?: string; fps?: number; width?: number; height?: number }): Promise<{ id: string; name: string; updatedAt: string }>;
  openProject(projectId: string): Promise<unknown>;
  closeProject(): Promise<boolean>;
  revealProjectDir(): Promise<boolean>;
  applyOps(ops: unknown[], groupLabel?: string): Promise<{ ok: boolean; seq: number; inverses: unknown[]; doc: unknown }>;
  undo(): Promise<{ ok: boolean; doc?: unknown }>;
  redo(): Promise<{ ok: boolean; doc?: unknown }>;
  historyLabels(): Promise<{ canUndo: boolean; canRedo: boolean; undoLabel: string | null }>;
  getAsset(assetId: string): Promise<unknown>;
  relinkAsset(assetId: string, newPath: string): Promise<unknown>;
  removeAsset(assetId: string): Promise<boolean>;
  listJobs(projectId?: string): Promise<unknown[]>;
  cancelJob(jobId: string): Promise<boolean>;
  exportPresets(): Promise<{ name: string; width: number; height: number; format: string; videoBitrateK: number }[]>;
  startExport(presetName: string): Promise<unknown>;
  cancelExport(exportId: string): Promise<boolean>;
  listExports(): Promise<unknown[]>;
  // hidden export-window helpers
  exportBundle(exportId: string): Promise<{ doc: unknown; mediaUrls: Record<string, string> }>;
  sendExportFrame(exportId: string, index: number, buffer: ArrayBuffer, width: number, height: number): void;
  sendExportDone(exportId: string): void;
  sendExportError(exportId: string, message: string): void;
  onEvent(handler: (envelope: { type: string; payload?: unknown }) => void): () => void;
  onMenuAction(handler: (action: { action: string }) => void): () => void;
}

const api: CutboardApi = {
  appInfo: () => ipcRenderer.invoke('app:info'),
  pickMediaFiles: () => ipcRenderer.invoke('dialog:pickMedia'),
  pickFilePath: () => ipcRenderer.invoke('dialog:pickFile'),
  listRecentProjects: () => ipcRenderer.invoke('projects:listRecent'),
  createProject: (input) => ipcRenderer.invoke('projects:create', input),
  openProject: (projectId) => ipcRenderer.invoke('projects:open', projectId),
  closeProject: () => ipcRenderer.invoke('projects:close'),
  revealProjectDir: () => ipcRenderer.invoke('projects:reveal'),
  applyOps: (ops, groupLabel) => ipcRenderer.invoke('ops:apply', { ops, groupLabel }),
  undo: () => ipcRenderer.invoke('history:undo'),
  redo: () => ipcRenderer.invoke('history:redo'),
  historyLabels: () => ipcRenderer.invoke('history:labels'),
  getAsset: (assetId) => ipcRenderer.invoke('assets:get', assetId),
  relinkAsset: (assetId, newPath) => ipcRenderer.invoke('assets:relink', { assetId, newPath }),
  removeAsset: (assetId) => ipcRenderer.invoke('assets:remove', assetId),
  listJobs: (projectId) => ipcRenderer.invoke('jobs:list', projectId),
  cancelJob: (jobId) => ipcRenderer.invoke('jobs:cancel', jobId),
  exportPresets: () => ipcRenderer.invoke('exports:presets'),
  startExport: (presetName) => ipcRenderer.invoke('exports:start', presetName),
  cancelExport: (exportId) => ipcRenderer.invoke('exports:cancel', exportId),
  listExports: () => ipcRenderer.invoke('exports:list'),
  exportBundle: (exportId) => ipcRenderer.invoke('export:bundle', exportId),
  sendExportFrame: (exportId, index, buffer, width, height) => {
    ipcRenderer.send('export:window:frame', exportId, index, buffer, width, height);
  },
  sendExportDone: (exportId) => ipcRenderer.send('export:window:done', exportId),
  sendExportError: (exportId, message) => ipcRenderer.send('export:window:error', exportId, message),
  onEvent: (handler) => {
    const listener = (_e: Electron.IpcRendererEvent, envelope: { type: string; payload?: unknown }) => handler(envelope);
    ipcRenderer.on('event', listener);
    return () => ipcRenderer.removeListener('event', listener);
  },
  onMenuAction: (handler) => {
    const listener = (_e: Electron.IpcRendererEvent, action: { action: string }) => handler(action);
    ipcRenderer.on('menu:action', listener);
    return () => ipcRenderer.removeListener('menu:action', listener);
  },
};

contextBridge.exposeInMainWorld('cutboard', api);
