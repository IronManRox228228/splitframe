import { app, BrowserWindow, dialog, ipcMain, shell } from 'electron';
import { join, resolve } from 'node:path';
import { z } from 'zod';
import { Op, opSchema, Actor } from '@cutboard/schema';
import { projectService } from './project-service.ts';
import { checkAssetAvailability, getAsset, importFiles, isMediaFile, relinkAsset, removeAsset, onAssetEvent } from './asset-service.ts';
import { exportService, EXPORT_PRESETS, registerExportWindowIpc, onExportEvent } from './export-service.ts';
import { jobs, JobRow, onJobEvent } from './jobs.ts';
import { getFfmpeg, FfmpegInfo } from './ffmpeg.ts';
import { getPaths } from './paths.ts';
import { editorContextCache } from './editor-context.ts';
import { callTool } from './tools-bridge.ts';
import { getSettings, saveSettings, rotateMcpToken } from './settings.ts';
import { startMcpServer, stopMcpServer, revokeMcpSessions, getMcpActivity } from './mcp-server.ts';
import { buildMcpSnippets } from './mcp-auth.ts';
import { listModels, downloadModel, deleteModel, cancelModelDownload, onModelProgress } from './models.ts';
import { searchWords, groupWordHits, searchScenes, isVectorSearchEnabled } from './analysis/search.ts';
import { groupHasAllTerms } from './analysis/search-query.ts';
import { getVlmConfig, setVlmConfig } from './analysis/vlm.ts';
import { sendChatMessage, abortChat } from './agent/chat.ts';

/**
 * IPC is the security boundary (addendum §5.4): the renderer is untrusted, every
 * handler validates its inputs with zod, and only the allowlisted channels below exist.
 */

const opsInput = z.object({
  ops: z.array(opSchema).min(1).max(500),
  groupLabel: z.string().max(120).optional(),
});

const chatSendInput = z.object({
  chatId: z.string().min(4).max(80),
  message: z.string().min(1).max(8000),
  history: z
    .array(z.object({ role: z.enum(['user', 'assistant']), content: z.string().max(8000) }))
    .max(200)
    .optional()
    .default([]),
});

// a sender without a window (closed mid-request) falls back to an app-modal dialog
const showOpen = (win: BrowserWindow | null, options: Electron.OpenDialogOptions) =>
  win ? dialog.showOpenDialog(win, options) : dialog.showOpenDialog(options);

const editorContextInput = z.object({
  selection: z.array(z.string().max(80)).max(500).optional(),
  playheadFrame: z.number().int().nonnegative().optional(),
  highlightedRange: z.object({ startFrame: z.number().int(), endFrame: z.number().int() }).nullable().optional(),
  openProjectId: z.string().max(80).optional(),
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
      version: app.getVersion(),
      platform: process.platform,
      projectsRoot: getPaths().projectsRoot,
      ffmpeg,
    };
  });

  ipcMain.handle('dialog:pickMedia', async (e) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const res = await showOpen(win, {
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
    const res = await showOpen(win, {
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
      // projectService.apply broadcasts doc:changed itself (single source of truth)
      const result = projectService.apply(parsed.ops, actorFor(e), parsed.groupLabel);
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
      if (!isMediaFile(parsed.newPath)) throw new Error('Choose a supported media file.');
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
  ipcMain.handle('exports:revealPath', (_e, path: string) => {
    z.string().min(1).parse(path);
    // only reveal files this app exported; the renderer must not probe arbitrary paths
    if (!exportService.list().some((row) => row.outputPath === path)) return false;
    shell.showItemInFolder(path);
    return true;
  });
  ipcMain.handle('exports:otio', async (_e) => {
    if (!projectService.isOpen) throw new Error('No project open');
    const { buildOtio } = await import('./analysis/otio.ts');
    const { saveOtio } = await import('./analysis/reference.ts');
    const { getAssets } = await import('./asset-service.ts');
    const doc = projectService.doc;
    const media = new Map<string, string>();
    for (const asset of getAssets(projectService.projectId)) {
      media.set(asset.id, asset.path); // link the original footage, never the 540p proxy in the cache
    }
    const json = buildOtio(doc, (assetId) => media.get(assetId) ?? null);
    const slug = doc.project.name.toLowerCase().replace(/[^a-z0-9]+/g, '-') || 'timeline';
    const path = await saveOtio(`${slug}-${Date.now()}`, json, projectService.dir);
    return { path };
  });

  // ---------- editor context (for agent tools: "the selected clip", "at this point") ----------
  ipcMain.on('editorContext:set', (_e, ctx: unknown) => {
    const parsed = editorContextInput.safeParse(ctx);
    if (parsed.success) editorContextCache.set(parsed.data);
  });

  // ---------- tools (shared front door for the built-in chat; MCP has its own) ----------
  ipcMain.handle(
    'tools:call',
    (e, input: { name: string; args?: unknown }) => {
      const parsed = z.object({ name: z.string().min(1), args: z.unknown().optional() }).parse(input);
      return callTool(parsed.name, parsed.args ?? {}, actorFor(e));
    },
  );

  // ---------- MCP server settings (addendum §4) ----------
  ipcMain.handle('mcp:getStatus', async () => {
    const settings = await getSettings();
    return {
      ...settings.mcp,
      connectedClients: getMcpActivity().slice(0, 5),
      activity: getMcpActivity(),
    };
  });
  ipcMain.handle(
    'mcp:setEnabled',
    async (_e, enabled: boolean) => {
      z.boolean().parse(enabled);
      await saveSettings({ mcp: { ...(await getSettings()).mcp, enabled } });
      const result = enabled ? await startMcpServer() : (stopMcpServer(), { port: (await getSettings()).mcp.port });
      // a server that could not start (port in use) must not stay "enabled" in settings
      if ('error' in result) await saveSettings({ mcp: { ...(await getSettings()).mcp, enabled: false } });
      broadcast('event', { type: 'mcp:status', payload: { enabled: enabled && !('error' in result) } });
      return result;
    },
  );
  ipcMain.handle('mcp:rotateToken', async () => {
    const token = await rotateMcpToken();
    revokeMcpSessions();
    return { token };
  });
  ipcMain.handle('mcp:snippets', async () => {
    const { mcp } = await getSettings();
    // the stdio shim ships with the app (extraResources when packaged, the workspace build in dev)
    const shimPath = app.isPackaged
      ? join(process.resourcesPath, 'mcp-shim', 'index.js')
      : resolve(app.getAppPath(), '../mcp-shim/dist/index.js');
    return buildMcpSnippets({ url: `http://127.0.0.1:${mcp.port}/mcp`, token: mcp.token, shimPath });
  });

  // ---------- built-in agent chat (Milestone 3) ----------
  // `.on` handlers must not throw (that becomes an uncaught main-process exception), so bad
  // input is dropped instead of parsed with .parse()
  ipcMain.on('chat:send', (_e, input: unknown) => {
    const parsed = chatSendInput.safeParse(input);
    if (!parsed.success) return;
    void sendChatMessage(parsed.data.chatId, parsed.data.message, parsed.data.history);
  });
  ipcMain.on('chat:abort', (_e, chatId: unknown) => {
    if (typeof chatId === 'string') abortChat(chatId);
  });

  // ---------- models / analysis / search (Milestone 2) ----------
  ipcMain.handle('models:list', () => listModels());
  ipcMain.handle('models:download', (_e, id: string) => downloadModel(id));
  ipcMain.handle('models:delete', (_e, id: string) => {
    deleteModel(id);
    return true;
  });
  ipcMain.handle('models:cancel', (_e, id: string) => {
    cancelModelDownload(id);
    return true;
  });
  ipcMain.handle(
    'search:query',
    async (_e, query: string) => {
      z.string().min(1).parse(query);
      const words = groupWordHits(searchWords(query)).filter((group) => groupHasAllTerms(group.text, query));
      const scenes = await searchScenes(query);
      return { words, scenes, vectorSearch: isVectorSearchEnabled() };
    },
  );
  ipcMain.handle('ai:getConfig', async () => {
    const settings = await getSettings();
    const vlm = await getVlmConfig();
    return {
      agentProvider: settings.ai?.agentProvider ?? 'anthropic',
      agentModel: settings.ai?.agentModel ?? '',
      vlmProvider: vlm.provider,
      vlmModel: vlm.model,
      ollamaUrl: vlm.ollamaUrl,
      llamacppUrl: settings.ai?.llamacppUrl ?? 'http://127.0.0.1:8080',
      asrModel: settings.asr?.model ?? 'base.en',
    };
  });
  ipcMain.handle(
    'ai:setConfig',
    (_e, patch: { agentProvider?: string; agentModel?: string; vlmProvider?: string; vlmModel?: string; ollamaUrl?: string; llamacppUrl?: string; llamacppKey?: string; agentKey?: string; anthropicKey?: string; openaiKey?: string }) => {
      const parsed = z.object({
        agentProvider: z.enum(['anthropic', 'openai', 'google', 'openrouter', 'ollama', 'llamacpp']).optional(),
        agentModel: z.string().max(120).optional(),
        vlmProvider: z.enum(['none', 'ollama', 'anthropic', 'openai']).optional(),
        vlmModel: z.string().max(120).optional(),
        ollamaUrl: z.string().url().optional(),
        llamacppUrl: z.string().url().optional(),
        agentKey: z.string().max(400).optional(),
        llamacppKey: z.string().max(400).optional(),
        anthropicKey: z.string().max(400).optional(),
        openaiKey: z.string().max(400).optional(),
      }).parse(patch);
      // use the validated copy: it drops keys the schema doesn't know instead of merging them into settings.json
      return setVlmConfig(parsed as never).then(() => true);
    },
  );

  // hidden export window channels
  registerExportWindowIpc();
}

/** Forward main-process events (jobs, assets, exports, models, MCP) to all windows. */
export function wireEvents(broadcast: (channel: string, payload: unknown) => void): void {
  onJobEvent((job: JobRow) => {
    broadcast('event', { type: 'job', payload: job });
    // system notifications (addendum §5.3): only when the window isn't focused
    if (job.status === 'done' && job.type === 'ingest-asset') {
      notifyUnlessFocused('Analysis complete', 'Footage is ready to edit.');
    }
  });
  onAssetEvent((asset) => broadcast('event', { type: 'asset', payload: asset }));
  onExportEvent((row) => {
    broadcast('event', { type: 'export', payload: row });
    if (row.status === 'done') notifyUnlessFocused('Export complete', row.outputPath ?? '');
    else if (row.status === 'failed') notifyUnlessFocused('Export failed', row.error ?? '');
  });
  onModelProgress((p) => broadcast('event', { type: 'model:progress', payload: p }));
}

function notifyUnlessFocused(title: string, body: string): void {
  void (async () => {
    const { Notification, BrowserWindow } = await import('electron');
    const focused = BrowserWindow.getAllWindows().some((w) => w.isFocused());
    if (focused || !Notification.isSupported()) return;
    new Notification({ title, body, silent: true }).show();
  })();
}
