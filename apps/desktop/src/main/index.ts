import { app, BrowserWindow, Menu, protocol, session, shell } from 'electron';
import { join } from 'node:path';
import { realpath } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { getDb, closeDb } from './db.ts';
import { getPaths } from './paths.ts';
import { jobs } from './jobs.ts';
import { registerIpc, wireEvents } from './ipc.ts';
import { projectService } from './project-service.ts';
import { setBroadcast } from './events.ts';
import { buildCsp, MOTION_SANDBOX_CSP } from './csp.ts';
import { canServeMediaPath } from './media-access.ts';
import { buildMotionSandboxHtml } from './motion-sandbox.ts';
import { isKnownAssetPath } from './asset-service.ts';

/**
 * Cutboard desktop entry. Security posture (addendum §5.4): renderer is untrusted —
 * contextIsolation on, nodeIntegration off, sandboxed preload, strict CSP, no
 * navigation/new windows, custom cbmedia:// protocol for local media access only.
 */

// must be called before app ready
protocol.registerSchemesAsPrivileged([
  { scheme: 'cbmedia', privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true } },
  // network-less document that evaluates generated motion-graphic code (see motion-sandbox.ts)
  { scheme: 'cbsandbox', privileges: { standard: true, secure: true } },
]);

let mainWindow: BrowserWindow | null = null;
const singleInstance = app.requestSingleInstanceLock();
if (!singleInstance) {
  app.quit();
}

export function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload);
  }
}
setBroadcast(broadcast);

function createMainWindow(): void {
  mainWindow = new BrowserWindow({
    width: 1600,
    height: 1000,
    minWidth: 1100,
    minHeight: 700,
    title: 'Cutboard',
    backgroundColor: '#0b0b0d',
    titleBarStyle: process.platform === 'darwin' ? 'hiddenInset' : 'default',
    trafficLightPosition: { x: 16, y: 14 },
    webPreferences: {
      preload: join(__dirname, '../preload/index.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
      spellcheck: false,
    },
  });

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  mainWindow.webContents.on('will-navigate', (e, url) => {
    if (!url.startsWith(getRendererOrigin())) e.preventDefault();
  });

  if (process.env['ELECTRON_RENDERER_URL']) {
    void mainWindow.loadURL(process.env['ELECTRON_RENDERER_URL']);
  } else {
    void mainWindow.loadFile(join(__dirname, '../renderer/index.html'));
  }
  mainWindow.on('closed', () => (mainWindow = null));

  // CI/smoke hook: CUTBOARD_SMOKE_CAPTURE=<path> saves a screenshot of the loaded
  // window (console + failures go to stderr) and quits. CUTBOARD_SMOKE_CLIP=<file>
  // additionally drives the full edit loop: create project → import → timeline →
  // export, writing a summary to stderr. No effect unless set.
  const capturePath = process.env['CUTBOARD_SMOKE_CAPTURE'];
  if (capturePath) {
    mainWindow.webContents.on('console-message', (_e, level, message, line, sourceId) => {
      process.stderr.write(`[renderer:${level}] ${message} (${sourceId}:${line})\n`);
    });
    mainWindow.webContents.on('did-fail-load', (_e, code, desc) => {
      process.stderr.write(`[did-fail-load] ${code} ${desc}\n`);
    });
    mainWindow.webContents.once('did-finish-load', () => {
      setTimeout(() => {
        void smokeRun(capturePath, process.env['CUTBOARD_SMOKE_CLIP']);
      }, 1500);
    });
  }
}

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

async function smokeRun(capturePath: string, clipPath: string | undefined): Promise<void> {
  const { writeFile } = await import('node:fs');
  const summary: Record<string, unknown> = {};
  try {
    if (clipPath) {
      const wc = mainWindow!.webContents;
      // create + open a project through the real store
      await wc.executeJavaScript(
        `(async () => {
          const s = window.__cutboardStore;
          await s.getState().createProject('Smoke test');
          const recents = await window.cutboard.listRecentProjects();
          await s.getState().openProject(recents[0].id);
          return s.getState().screen;
        })()`,
        true,
      ).then((r) => process.stderr.write(`[smoke] screen after open: ${JSON.stringify(r)}\n`));
      // wait for the project service to see it open (renderer went through IPC)
      const { projectService } = await import('./project-service.ts');
      for (let i = 0; i < 50 && !projectService.isOpen; i++) await sleep(100);
      if (!projectService.isOpen) throw new Error('project never opened');
      summary['project'] = projectService.projectId;

      // import the clip main-side (no dialogs) and wait for ingest to finish
      const { importFiles, getAssets } = await import('./asset-service.ts');
      const imported = await importFiles([clipPath]);
      if (imported.length === 0) throw new Error('import produced no assets');
      const assetId = imported[0]!.id;
      for (let i = 0; i < 300; i++) {
        const asset = getAssets(projectService.projectId).find((a) => a.id === assetId);
        if (asset?.status === 'analyzed' || asset?.status === 'failed') {
          if (asset.status === 'failed') throw new Error(`ingest failed: ${asset.error}`);
          summary['asset'] = { kind: asset.kind, durationMs: asset.durationMs, proxy: Boolean(asset.proxyPath) };
          break;
        }
        await sleep(100);
      }

      // add it to the timeline via the store (same code path as double-click in the UI)
      await wc.executeJavaScript(
        `(async () => {
          const s = window.__cutboardStore;
          await s.getState().addAssetToTimeline(${JSON.stringify(assetId)}, 0);
          return { screen: s.getState().screen, items: s.getState().doc?.items.length ?? -1 };
        })()`,
        true,
      ).then((r) => process.stderr.write(`[smoke] after add: ${JSON.stringify(r)}\n`));
      await sleep(800);

      // exercise the tool pipeline through the front door (same as chat/MCP)
      const toolResults: Record<string, unknown> = {};
      const call = async (name: string, args: unknown) =>
        wc.executeJavaScript(`window.cutboard.callTool(${JSON.stringify(name)}, ${JSON.stringify(args)})`, true);
      try {
        toolResults['captions'] = await call('addCaptions', { preset: 'karaoke' });
        toolResults['silences'] = await call('removeSilences', { thresholdSec: 0.4 });
      } catch (err) {
        toolResults['macroError'] = err instanceof Error ? err.message : String(err);
      }
      summary['tools'] = toolResults;
      await sleep(1000);
      summary['items'] = projectService.doc.items.length;
      // make sure the window is actually on the editor screen before capturing
      for (let i = 0; i < 20; i++) {
        const screen = await wc.executeJavaScript(`window.__cutboardStore?.getState().screen`);
        if (screen === 'editor') break;
        await wc.executeJavaScript(
          `(async () => { const s = window.__cutboardStore; const r = await window.cutboard.listRecentProjects(); await s.getState().openProject(r[0].id); })()`,
          true,
        );
        await sleep(500);
      }
      const editorShot = await wc.capturePage();
      writeFile(capturePath.replace(/\.png$/, '-editor.png'), editorShot.toPNG(), () => undefined);

      // beat sync against a click track, if provided
      const beatPath = process.env['CUTBOARD_SMOKE_BEAT'];
      if (beatPath) {
        try {
          const beatAsset = (await importFiles([beatPath]))[0]!;
          for (let i = 0; i < 300; i++) {
            const a = getAssets(projectService.projectId).find((x) => x.id === beatAsset.id);
            if (a?.status === 'analyzed' || a?.status === 'failed') break;
            await sleep(100);
          }
          await wc.executeJavaScript(
            `(async () => { const s = window.__cutboardStore; await s.getState().addAssetToTimeline(${JSON.stringify(beatAsset.id)}, 0); })()`,
            true,
          );
          await sleep(500);
          const beats = await call('analyzeBeats', { assetId: beatAsset.id });
          const beatMap = beats as { bpm?: number };
          const musicItem = projectService.doc.items.find((i) => i.assetId === beatAsset.id);
          const sync = await call('beatSync', { musicItemId: musicItem!.id });
          toolResults['beats'] = { bpm: beatMap.bpm, sync };
        } catch (err) {
          toolResults['beatError'] = err instanceof Error ? err.message : String(err);
        }
      }

      // export through the real service (hidden render window + ffmpeg)
      const { exportService } = await import('./export-service.ts');
      const started = Date.now();
      const row = await exportService.start('YouTube 1080p');
      for (let i = 0; i < 1200; i++) {
        const cur = exportService.get(row.id);
        if (cur && (cur.status === 'done' || cur.status === 'failed' || cur.status === 'cancelled')) {
          summary['export'] = { status: cur.status, ms: Date.now() - started, output: cur.outputPath, error: cur.error };
          break;
        }
        await sleep(500);
      }
    }
  } catch (err) {
    summary['error'] = err instanceof Error ? err.message : String(err);
  } finally {
    const shot = await mainWindow!.webContents.capturePage();
    writeFile(capturePath, shot.toPNG(), () => {
      process.stderr.write(`[smoke] ${JSON.stringify(summary)}\n`);
      app.quit();
    });
  }
}

function getRendererOrigin(): string {
  return process.env['ELECTRON_RENDERER_URL'] ?? 'file://';
}

function buildMenu(): void {
  const send = (action: string) => () => mainWindow?.webContents.send('menu:action', { action });
  const isMac = process.platform === 'darwin';
  const template: Electron.MenuItemConstructorOptions[] = [
    ...(isMac ? [{ role: 'appMenu' as const }] : []),
    {
      label: 'File',
      submenu: [
        { label: 'Import Media…', accelerator: 'CmdOrCtrl+I', click: send('import') },
        { label: 'Save Project', accelerator: 'CmdOrCtrl+S', click: send('save') },
        { type: 'separator' },
        { role: 'close' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        { label: 'Undo', accelerator: 'CmdOrCtrl+Z', click: send('undo') },
        { label: 'Redo', accelerator: 'Shift+CmdOrCtrl+Z', click: send('redo') },
        { type: 'separator' },
        { role: 'cut' },
        { role: 'copy' },
        { role: 'paste' },
        { role: 'selectAll' },
      ],
    },
    {
      label: 'Timeline',
      submenu: [
        // plain-key shortcuts (S, Backspace, ⌘D) are handled by the renderer's keydown
        // handler so they never fire while typing in inputs.
        { label: 'Split at Playhead', click: send('split') },
        { label: 'Delete Selected', click: send('delete') },
        { label: 'Clone Selected', click: send('clone') },
        { type: 'separator' },
        { label: 'Zoom In', accelerator: 'CmdOrCtrl+=', click: send('zoomIn') },
        { label: 'Zoom Out', accelerator: 'CmdOrCtrl+-', click: send('zoomOut') },
        { label: 'Zoom to Fit', accelerator: 'Shift+Z', click: send('zoomFit') },
      ],
    },
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
      ],
    },
    {
      role: 'help',
      submenu: [
        {
          label: 'Learn More',
          click: () => void shell.openExternal('https://github.com/'),
        },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function registerSandboxProtocol(): void {
  const html = buildMotionSandboxHtml();
  protocol.handle('cbsandbox', () => new Response(html, {
    headers: {
      'content-type': 'text/html; charset=utf-8',
      'content-security-policy': MOTION_SANDBOX_CSP,
      'x-content-type-options': 'nosniff',
    },
  }));
}

function registerMediaProtocol(): void {
  protocol.handle('cbmedia', async (request) => {
    try {
      const url = new URL(request.url);
      // cbmedia://media/<encodeURIComponent(absolute path)>
      const encoded = url.href.replace(/^cbmedia:\/\/media\//, '');
      const filePath = decodeURIComponent(encoded);
      if (!filePath.startsWith('/') && !/^[a-zA-Z]:\\/.test(filePath)) {
        return new Response('Invalid path', { status: 400 });
      }
      // the renderer is untrusted: serve only project folders and registered assets, and
      // check again after resolving symlinks so a link inside a project cannot escape
      const roots = [getPaths().projectsRoot];
      const real = await realpath(filePath).catch(() => null);
      if (!real) return new Response('Not found', { status: 404 });
      const realRoots = await Promise.all(roots.map((r) => realpath(r).catch(() => r)));
      if (!canServeMediaPath(filePath, real, { roots, realRoots, isKnownAssetPath })) {
        return new Response('Forbidden', { status: 403 });
      }
      const fileUrl = pathToFileURL(filePath).href;
      const { net } = await import('electron');
      // forward Range so media elements can seek without re-reading the whole file
      const range = request.headers.get('range');
      const res = await net.fetch(fileUrl, range ? { headers: { range } } : undefined);
      // explicit content-type + CORS so crossOrigin=anonymous media elements (and canvas
      // readback in the compositor) work from any window
      const headers = new Headers(res.headers);
      if (!headers.has('content-type')) {
        const ext = filePath.split('.').pop()?.toLowerCase() ?? '';
        const mime =
          ext === 'mp4' || ext === 'm4v' ? 'video/mp4'
          : ext === 'mov' ? 'video/quicktime'
          : ext === 'webm' ? 'video/webm'
          : ext === 'mkv' ? 'video/x-matroska'
          : ext === 'png' ? 'image/png'
          : ext === 'jpg' || ext === 'jpeg' ? 'image/jpeg'
          : ext === 'mp3' ? 'audio/mpeg'
          : ext === 'wav' ? 'audio/wav'
          : ext === 'm4a' ? 'audio/mp4'
          : ext === 'gif' ? 'image/gif'
          : 'application/octet-stream';
        headers.set('content-type', mime);
      }
      headers.set('access-control-allow-origin', '*');
      return new Response(res.body, { status: res.status, headers });
    } catch (err) {
      return new Response(`cbmedia error: ${err instanceof Error ? err.message : String(err)}`, { status: 500 });
    }
  });
}

app.on('second-instance', () => {
  if (mainWindow) {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  }
});

void app.whenReady().then(() => {
  // security headers for the renderer session (single source of truth; dev allows the
  // inline scripts vite's react plugin injects, prod stays strict)
  const isDev = Boolean(process.env['ELECTRON_RENDERER_URL']);
  const csp = buildCsp({ dev: isDev });
  session.defaultSession.webRequest.onHeadersReceived((details, callback) => {
    // the motion sandbox carries its own, stricter policy
    if (details.url.startsWith('cbsandbox:')) return callback({});
    callback({
      responseHeaders: {
        ...details.responseHeaders,
        'Content-Security-Policy': [csp],
      },
    });
  });

  registerMediaProtocol();
  registerSandboxProtocol();
  getDb();
  registerIpc(broadcast);
  wireEvents(broadcast);
  buildMenu();
  createMainWindow();
  jobs.resumePending();

  void (async () => {
    const { initSearchSchema } = await import('./analysis/search.ts');
    initSearchSchema();
  })();

  // local MCP server (addendum §4): start only when the user enabled it
  void (async () => {
    const { getSettings } = await import('./settings.ts');
    const { startMcpServer } = await import('./mcp-server.ts');
    const settings = await getSettings();
    if (settings.mcp.enabled) await startMcpServer();
  })();

  // auto-update (addendum §3): opt-in via env until a signed release feed exists
  if (app.isPackaged && process.env['CUTBOARD_AUTOUPDATE'] === '1') {
    void (async () => {
      const { autoUpdater } = await import('electron-updater');
      autoUpdater.checkForUpdatesAndNotify().catch((err) => {
        process.stderr.write(`[updater] ${err instanceof Error ? err.message : String(err)}\n`);
      });
    })();
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createMainWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});

app.on('before-quit', () => {
  // safe shutdown: SQLite WAL flushes on close
  try {
    projectService.close();
    closeDb();
  } catch {
    /* already closed */
  }
});

void getPaths; // ensure paths initialized on first use
