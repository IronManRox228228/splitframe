import { contextBridge, ipcRenderer } from 'electron';

/**
 * Typed, allowlisted bridge. The renderer never touches Node — every capability is an
 * explicit invoke channel validated again in the main process (addendum §5.4).
 */
export interface ProjectSummary {
  id: string;
  name: string;
  updatedAt: string;
  width: number;
  height: number;
  fps: number;
  durationMs: number;
  thumbPath: string | null;
}

export interface ExportStartOptions {
  /** one of the platform presets by name; otherwise quality + format are used */
  presetName?: string;
  quality?: '720p' | '1080p' | '1440p';
  format?: 'mp4' | 'webm';
  fileName?: string;
}

export interface ExportFolder {
  dir: string;
  label: string;
}

export interface CutboardApi {
  appInfo(): Promise<{
    version: string;
    platform: string;
    projectsRoot: string;
    ffmpeg: { ffmpegPath: string; ffprobePath: string; source: string; h264Encoder: string; version: string } | null;
  }>;
  pickMediaFiles(): Promise<unknown[]>;
  pickFilePath(): Promise<string | null>;
  listRecentProjects(): Promise<ProjectSummary[]>;
  renameProject(projectId: string, name: string): Promise<boolean>;
  duplicateProject(projectId: string): Promise<{ id: string; name: string; updatedAt: string }>;
  deleteProject(projectId: string): Promise<boolean>;
  revealProjectById(projectId: string): Promise<boolean>;
  createProject(input?: { name?: string; fps?: number; width?: number; height?: number }): Promise<{ id: string; name: string; updatedAt: string }>;
  openProject(projectId: string): Promise<unknown>;
  closeProject(): Promise<boolean>;
  revealProjectDir(): Promise<boolean>;
  applyOps(ops: unknown[], groupLabel?: string): Promise<{ ok: boolean; seq: number; inverses: unknown[]; doc: unknown }>;
  undo(): Promise<{ ok: boolean; doc?: unknown }>;
  redo(): Promise<{ ok: boolean; doc?: unknown }>;
  historyLabels(): Promise<{ canUndo: boolean; canRedo: boolean; undoLabel: string | null }>;
  getAsset(assetId: string): Promise<unknown>;
  checkAssetAvailability(assetId: string): Promise<'ok' | 'missing'>;
  relinkAsset(assetId: string, newPath: string): Promise<unknown>;
  removeAsset(assetId: string): Promise<boolean>;
  listJobs(projectId?: string): Promise<unknown[]>;
  cancelJob(jobId: string): Promise<boolean>;
  exportPresets(): Promise<{ name: string; width: number; height: number; format: string; videoBitrateK: number }[]>;
  startExport(options: string | ExportStartOptions): Promise<unknown>;
  getExportFolder(): Promise<ExportFolder>;
  chooseExportFolder(): Promise<ExportFolder | null>;
  openExportedFile(path: string): Promise<boolean>;
  cancelExport(exportId: string): Promise<boolean>;
  listExports(): Promise<unknown[]>;
  revealExportPath(path: string): Promise<boolean>;
  exportOtio(): Promise<{ path: string }>;
  setEditorContext(ctx: { selection?: string[]; playheadFrame?: number; highlightedRange?: { startFrame: number; endFrame: number } | null; openProjectId?: string }): void;
  callTool(name: string, args?: unknown): Promise<unknown>;
  mcpGetStatus(): Promise<unknown>;
  mcpSetEnabled(enabled: boolean): Promise<unknown>;
  mcpRotateToken(): Promise<{ token: string }>;
  mcpSnippets(): Promise<{ url: string; claudeCode: string; claudeDesktop: string; codex: string; cursor: string }>;
  listModels(): Promise<unknown[]>;
  downloadModel(id: string): Promise<{ ok: boolean; error?: string }>;
  deleteModel(id: string): Promise<boolean>;
  cancelModelDownload(id: string): Promise<boolean>;
  searchQuery(query: string): Promise<{ words: unknown[]; scenes: unknown[]; vectorSearch: boolean }>;
  aiGetConfig(): Promise<unknown>;
  aiSetConfig(patch: Record<string, unknown>): Promise<boolean>;
  sendChat(chatId: string, message: string, history?: { role: 'user' | 'assistant'; content: string }[]): void;
  abortChat(chatId: string): void;
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
  renameProject: (projectId, name) => ipcRenderer.invoke('projects:rename', { projectId, name }),
  duplicateProject: (projectId) => ipcRenderer.invoke('projects:duplicate', projectId),
  deleteProject: (projectId) => ipcRenderer.invoke('projects:delete', projectId),
  revealProjectById: (projectId) => ipcRenderer.invoke('projects:revealById', projectId),
  createProject: (input) => ipcRenderer.invoke('projects:create', input),
  openProject: (projectId) => ipcRenderer.invoke('projects:open', projectId),
  closeProject: () => ipcRenderer.invoke('projects:close'),
  revealProjectDir: () => ipcRenderer.invoke('projects:reveal'),
  applyOps: (ops, groupLabel) => ipcRenderer.invoke('ops:apply', { ops, groupLabel }),
  undo: () => ipcRenderer.invoke('history:undo'),
  redo: () => ipcRenderer.invoke('history:redo'),
  historyLabels: () => ipcRenderer.invoke('history:labels'),
  getAsset: (assetId) => ipcRenderer.invoke('assets:get', assetId),
  checkAssetAvailability: (assetId) => ipcRenderer.invoke('assets:checkAvailability', assetId),
  relinkAsset: (assetId, newPath) => ipcRenderer.invoke('assets:relink', { assetId, newPath }),
  removeAsset: (assetId) => ipcRenderer.invoke('assets:remove', assetId),
  listJobs: (projectId) => ipcRenderer.invoke('jobs:list', projectId),
  cancelJob: (jobId) => ipcRenderer.invoke('jobs:cancel', jobId),
  exportPresets: () => ipcRenderer.invoke('exports:presets'),
  startExport: (options) => ipcRenderer.invoke('exports:start', options),
  getExportFolder: () => ipcRenderer.invoke('exports:getFolder'),
  chooseExportFolder: () => ipcRenderer.invoke('exports:chooseFolder'),
  openExportedFile: (path) => ipcRenderer.invoke('exports:openFile', path),
  cancelExport: (exportId) => ipcRenderer.invoke('exports:cancel', exportId),
  listExports: () => ipcRenderer.invoke('exports:list'),
  revealExportPath: (path) => ipcRenderer.invoke('exports:revealPath', path),
  exportOtio: () => ipcRenderer.invoke('exports:otio'),
  setEditorContext: (ctx) => ipcRenderer.send('editorContext:set', ctx),
  callTool: (name, args) => ipcRenderer.invoke('tools:call', { name, args }),
  mcpGetStatus: () => ipcRenderer.invoke('mcp:getStatus'),
  mcpSetEnabled: (enabled) => ipcRenderer.invoke('mcp:setEnabled', enabled),
  mcpRotateToken: () => ipcRenderer.invoke('mcp:rotateToken'),
  mcpSnippets: () => ipcRenderer.invoke('mcp:snippets'),
  listModels: () => ipcRenderer.invoke('models:list'),
  downloadModel: (id) => ipcRenderer.invoke('models:download', id),
  deleteModel: (id) => ipcRenderer.invoke('models:delete', id),
  cancelModelDownload: (id) => ipcRenderer.invoke('models:cancel', id),
  searchQuery: (query) => ipcRenderer.invoke('search:query', query),
  aiGetConfig: () => ipcRenderer.invoke('ai:getConfig'),
  aiSetConfig: (patch) => ipcRenderer.invoke('ai:setConfig', patch),
  sendChat: (chatId, message, history) => ipcRenderer.send('chat:send', { chatId, message, history }),
  abortChat: (chatId) => ipcRenderer.send('chat:abort', chatId),
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
