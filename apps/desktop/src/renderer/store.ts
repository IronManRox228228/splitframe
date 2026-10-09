import { create } from 'zustand';
import {
  Asset,
  Item,
  ItemType,
  Op,
  TimelineDoc,
  newId,
} from '@cutboard/schema';
import { createItem, snapFrame, getSnapCandidates, docDurationFrames } from '@cutboard/editor-core';
import type { MediaPool } from './lib/media.ts';
import type { ProjectSummary } from '../preload/index.ts';
import { assetsToMap } from './lib/media.ts';
import { HEADER_W } from './lib/timeline-math.ts';
import { AGENT_MODE_IDS, type AgentModeId } from '../shared/agent-cards.ts';

/**
 * Renderer state. The main process owns the timeline; this store mirrors the doc for
 * rendering and issues ops over IPC. Undo/redo live in the main process (headless-safe).
 */

export interface ExportRowInfo {
  id: string;
  status: string;
  progress: number;
  outputPath: string | null;
  error: string | null;
  preset: { name: string };
  createdAt?: string;
  updatedAt?: string;
  frames?: { done: number; total: number };
  sizeBytes?: number;
}

export interface JobInfo {
  id: string;
  projectId: string | null;
  type: string;
  payload: Record<string, unknown>;
  status: 'pending' | 'running' | 'done' | 'failed' | 'cancelled';
  progress: number;
  stage: string | null;
  error: string | null;
}

export interface EditorState {
  screen: 'projects' | 'editor';
  appInfo: {
    version: string;
    platform: string;
    projectsRoot: string;
    ffmpeg: { source: string; h264Encoder: string; version: string } | null;
  } | null;
  recentProjects: ProjectSummary[];

  doc: TimelineDoc | null;
  assets: Asset[];
  jobs: Record<string, JobInfo>;
  exportsList: ExportRowInfo[];
  mediaPool: MediaPool | null;

  selection: string[];
  playhead: number;
  playing: boolean;
  pxPerFrame: number;
  /** bumps on every zoom-to-fit so the timeline can reset its scroll */
  fitNonce: number;
  snapEnabled: boolean;
  rippleEnabled: boolean;
  exportDialogOpen: boolean;
  /** the export started from the dialog (drives its status view and the completion toast) */
  watchedExportId: string | null;
  watchExport(id: string | null): void;
  toasts: Toast[];
  /** open left-rail panel (null = collapsed) */
  leftPanel: LeftPanelId | null;
  /** a message from the command bar waiting for the Assistant to send it */
  assistantOutbox: { text: string; nonce: number } | null;
  previewMuted: boolean;
  /** how much the assistant may do without asking (per project, kept by the main process) */
  agentMode: AgentModeId;
  /** the classic harness ignores modes; the pill says so */
  agentHarness: 'v1' | 'classic';
  /** clips a pending confirmation would change: the timeline outlines them */
  previewHighlight: string[];
  loadAgentMode(): Promise<void>;
  setAgentMode(mode: AgentModeId): Promise<void>;
  cycleAgentMode(): Promise<void>;
  setPreviewHighlight(ids: string[]): void;
  setLeftPanel(panel: LeftPanelId | null): void;
  sendToAssistant(text: string): void;
  clearAssistantOutbox(): void;
  setPreviewMuted(muted: boolean): void;

  bootstrap(): Promise<void>;
  refreshRecents(): Promise<void>;
  createProject(name?: string, size?: { width: number; height: number }): Promise<void>;
  openProject(id: string): Promise<void>;
  closeProject(): Promise<void>;
  revealProjectDir(): Promise<void>;
  importMedia(): Promise<void>;
  /** import OS paths (drag-and-drop); main re-validates them */
  importPaths(paths: string[]): Promise<void>;
  reanalyzeAsset(assetId: string): Promise<void>;
  /** remove an asset (and optionally the timeline clips using it, as one undoable group) from the open project */
  removeAssetFromProject(assetId: string, removeClips: boolean): Promise<void>;
  handleEvent(envelope: { type: string; payload?: unknown }): void;
  applyOps(ops: Op[], groupLabel?: string): Promise<void>;
  undo(): Promise<void>;
  redo(): Promise<void>;
  setPlayhead(frame: number): void;
  stepFrames(n: number): void;
  setPlaying(playing: boolean): void;
  togglePlay(): void;
  setPxPerFrame(px: number): void;
  zoomFit(viewportFrames: number): void;
  toggleSnap(): void;
  toggleRipple(): void;
  select(itemId: string | null, additive?: boolean): void;
  setSelection(ids: string[]): void;
  deleteSelection(): Promise<void>;
  splitAtPlayhead(): Promise<void>;
  cloneSelection(): Promise<void>;
  addAssetToTimeline(assetId: string, atFrame?: number, trackId?: string): Promise<void>;
  setExportDialog(open: boolean): void;
  showToast(message: string, opts?: ToastOptions): void;
  dismissToast(id: number): void;
}

export type LeftPanelId = 'media' | 'text' | 'audio' | 'captions' | 'assistant';
export type ToastKind = 'info' | 'success' | 'error';
export interface ToastAction {
  label: string;
  run(): void;
}
export interface Toast {
  id: number;
  kind: ToastKind;
  message: string;
  action?: ToastAction;
}
export interface ToastOptions {
  kind?: ToastKind;
  action?: ToastAction;
  durationMs?: number;
}
let toastSeq = 0;

const DEFAULT_PX_PER_FRAME = 3;

/** assets removed this session; late asset events for them must not resurrect the card */
const removedAssetIds = new Set<string>();

/** Strip Electron's "Error invoking remote method ..." wrapper so toasts read as plain sentences. */
const plainError = (err: unknown): string =>
  (err instanceof Error ? err.message : String(err)).replace(/^Error invoking remote method '[^']+': (Error: )?/, '');

const nameList = (names: string[]): string => (names.length <= 2 ? names.join(', ') : `${names.slice(0, 2).join(', ')} +${names.length - 2} more`);

/** Merge imported assets into the panel and show one summary toast. */
function mergeImport(summary: { assets: unknown[]; imported: number; skipped: string[]; failed: { name: string; error: string }[] }): void {
  const s = useEditor.getState();
  // asset events for these files can arrive before the call returns; merge by id so the panel
  // doesn't show a second, never-updated copy
  const byId = new Map(s.assets.map((a) => [a.id, a]));
  for (const a of summary.assets as Asset[]) if (!byId.has(a.id)) byId.set(a.id, a);
  useEditor.setState({ assets: [...byId.values()] });
  const parts: string[] = [];
  if (summary.imported > 0) parts.push(`Imported ${summary.imported} file${summary.imported === 1 ? '' : 's'}`);
  if (summary.skipped.length > 0) parts.push(`${summary.skipped.length} unsupported (${nameList(summary.skipped)})`);
  if (summary.failed.length > 0) parts.push(`${summary.failed.length} failed (${nameList(summary.failed.map((f) => f.name))})`);
  if (parts.length === 0) return;
  const bad = summary.skipped.length + summary.failed.length;
  const kind: ToastKind = bad === 0 ? 'success' : summary.imported === 0 ? 'error' : 'info';
  s.showToast(parts.join(' · '), { kind, durationMs: bad > 0 ? 7000 : undefined });
}

export const useEditor = create<EditorState>((set, get) => ({
  screen: 'projects',
  appInfo: null,
  recentProjects: [],
  doc: null,
  assets: [],
  jobs: {},
  exportsList: [],
  mediaPool: null,
  selection: [],
  playhead: 0,
  playing: false,
  pxPerFrame: DEFAULT_PX_PER_FRAME,
  fitNonce: 0,
  snapEnabled: true,
  rippleEnabled: false,
  exportDialogOpen: false,
  watchedExportId: null,
  watchExport(id) {
    set({ watchedExportId: id });
  },
  toasts: [],
  leftPanel: 'media',
  assistantOutbox: null,
  previewMuted: false,
  agentMode: 'default',
  agentHarness: 'v1',
  previewHighlight: [],
  async loadAgentMode() {
    try {
      const r = await window.cutboard.getAgentMode();
      set({ agentMode: r.mode, agentHarness: r.harness });
    } catch {
      /* keep the last known mode */
    }
  },
  async setAgentMode(mode) {
    set({ agentMode: mode });
    try {
      await window.cutboard.setAgentMode(mode);
    } catch {
      void get().loadAgentMode();
    }
  },
  async cycleAgentMode() {
    const i = AGENT_MODE_IDS.indexOf(get().agentMode);
    await get().setAgentMode(AGENT_MODE_IDS[(i + 1) % AGENT_MODE_IDS.length]!);
  },
  setPreviewHighlight(ids) {
    const cur = get().previewHighlight;
    if (ids.length === cur.length && ids.every((x, i) => x === cur[i])) return;
    set({ previewHighlight: ids });
  },
  setLeftPanel(panel) {
    set({ leftPanel: panel });
  },
  sendToAssistant(text) {
    set({ leftPanel: 'assistant', assistantOutbox: { text, nonce: Date.now() } });
  },
  clearAssistantOutbox() {
    set({ assistantOutbox: null });
  },
  setPreviewMuted(muted) {
    set({ previewMuted: muted });
  },

  async bootstrap() {
    const appInfo = await window.cutboard.appInfo();
    set({ appInfo });
    await get().refreshRecents();
    window.cutboard.onEvent((envelope) => get().handleEvent(envelope));
    window.cutboard.onMenuAction(({ action }) => {
      const s = get();
      // the Edit menu owns Ctrl/Cmd+Z; inside a text field it must undo the text, not the timeline
      const el = document.activeElement as HTMLElement | null;
      const typing = Boolean(el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable));
      if ((action === 'undo' || action === 'redo') && typing) document.execCommand(action);
      else if (action === 'undo') void s.undo();
      else if (action === 'redo') void s.redo();
      else if (action === 'import') void s.importMedia();
      else if (action === 'split') void s.splitAtPlayhead();
      else if (action === 'delete') void s.deleteSelection();
      else if (action === 'clone') void s.cloneSelection();
      else if (action === 'zoomIn') s.setPxPerFrame(s.pxPerFrame * 1.25);
      else if (action === 'zoomOut') s.setPxPerFrame(s.pxPerFrame / 1.25);
      else if (action === 'zoomFit') {
        const doc = s.doc;
        if (doc) s.zoomFit(Math.max(1, docDurationFrames(doc)));
      } else if (action === 'save') {
        s.showToast('Every edit is saved automatically (SQLite WAL).');
      }
    });
  },

  async refreshRecents() {
    set({ recentProjects: await window.cutboard.listRecentProjects() });
  },

  async createProject(name, size) {
    // no name: the main process picks "Untitled video", numbered when taken
    const project = await window.cutboard.createProject({ ...(name ? { name } : {}), ...size });
    await get().refreshRecents();
    await get().openProject(project.id);
  },

  async openProject(id) {
    const bundle = (await window.cutboard.openProject(id)) as {
      doc: TimelineDoc;
      assets: Asset[];
    };
    const { MediaPool: Pool } = await import('./lib/media.ts');
    set({
      screen: 'editor',
      doc: bundle.doc,
      assets: bundle.assets,
      mediaPool: new Pool(assetsToMap(bundle.assets), bundle.doc.project.fps),
      selection: [],
      playhead: 0,
      playing: false,
      pxPerFrame: DEFAULT_PX_PER_FRAME,
      jobs: {},
    });
    void get().loadAgentMode();
    const jobsList = (await window.cutboard.listJobs(id)) as JobInfo[];
    set({ jobs: Object.fromEntries(jobsList.map((j) => [j.id, j])) });
    // flag originals that moved or were deleted since last time (main emits an asset event for each change)
    for (const asset of bundle.assets) {
      if (asset.status === 'analyzed' || asset.status === 'missing') void window.cutboard.checkAssetAvailability(asset.id);
    }
  },

  async closeProject() {
    await window.cutboard.closeProject();
    get().mediaPool?.dispose();
    set({ screen: 'projects', doc: null, assets: [], mediaPool: null, selection: [], playing: false });
    await get().refreshRecents();
  },

  async revealProjectDir() {
    await window.cutboard.revealProjectDir();
  },

  async importMedia() {
    try {
      const summary = await window.cutboard.pickMediaFiles();
      if (summary) mergeImport(summary);
    } catch (err) {
      get().showToast(plainError(err), { kind: 'error' });
    }
  },

  async importPaths(paths) {
    if (paths.length === 0) return;
    try {
      mergeImport(await window.cutboard.importPaths(paths));
    } catch (err) {
      get().showToast(plainError(err), { kind: 'error' });
    }
  },

  async reanalyzeAsset(assetId) {
    try {
      await window.cutboard.reanalyzeAsset(assetId);
    } catch (err) {
      get().showToast(plainError(err), { kind: 'error' });
    }
  },

  async removeAssetFromProject(assetId, removeClips) {
    const { doc } = get();
    if (!doc) return;
    const itemIds = doc.items.filter((i) => i.assetId === assetId).map((i) => i.id);
    if (itemIds.length > 0) {
      if (!removeClips) return;
      await get().applyOps([{ type: 'item.remove', itemIds, ripple: false }], 'Remove media from timeline');
      if (get().doc?.items.some((i) => i.assetId === assetId)) return; // applyOps already toasted the error
    }
    try {
      await window.cutboard.removeAsset(assetId);
    } catch (err) {
      get().showToast(plainError(err), { kind: 'error' });
      return;
    }
    removedAssetIds.add(assetId);
    const gone = new Set(itemIds);
    set({ assets: get().assets.filter((a) => a.id !== assetId), selection: get().selection.filter((id) => !gone.has(id)) });
    get().showToast('Removed from project. Your original file was not touched.');
  },

  handleEvent(envelope) {
    const s = get();
    switch (envelope.type) {
      case 'doc:changed': {
        const payload = envelope.payload as { doc: TimelineDoc } | undefined;
        if (payload?.doc && s.doc && payload.doc.project.id === s.doc.project.id) {
          set({ doc: payload.doc });
        }
        break;
      }
      case 'agent:mode': {
        const mode = (envelope.payload as { mode?: AgentModeId } | undefined)?.mode;
        if (mode && AGENT_MODE_IDS.includes(mode)) set({ agentMode: mode });
        break;
      }
      case 'asset': {
        const asset = envelope.payload as Asset;
        if (removedAssetIds.has(asset.id)) break; // a cancelled job can still emit one last update
        // background jobs of another project can still be finishing; keep them out of this editor
        if (s.doc && asset.projectId !== s.doc.project.id) break;
        const assets = [...s.assets];
        const idx = assets.findIndex((a) => a.id === asset.id);
        if (idx === -1) assets.push(asset);
        else assets[idx] = asset;
        s.mediaPool?.updateAssets([asset]);
        set({ assets });
        break;
      }
      case 'job': {
        const job = envelope.payload as JobInfo;
        if (s.doc && job.projectId && job.projectId !== s.doc.project.id) break;
        set({ jobs: { ...s.jobs, [job.id]: job } });
        break;
      }
      case 'export': {
        const row = envelope.payload as { id: string; status: string; progress: number; outputPath: string | null; error: string | null; preset: { name: string } };
        const list = [...s.exportsList];
        const idx = list.findIndex((e) => e.id === row.id);
        if (idx === -1) list.unshift(row);
        else list[idx] = row;
        set({ exportsList: list });
        if (row.id === s.watchedExportId && !s.exportDialogOpen) {
          if (row.status === 'done') {
            const path = row.outputPath ?? '';
            s.showToast('Export finished', {
              kind: 'success',
              action: { label: 'Open', run: () => void window.cutboard.openExportedFile(path) },
            });
            set({ watchedExportId: null });
          } else if (row.status === 'failed') {
            s.showToast(`Export failed: ${(row.error ?? 'unknown error').slice(0, 200)}`, { kind: 'error' });
            set({ watchedExportId: null });
          } else if (row.status === 'cancelled') {
            set({ watchedExportId: null });
          }
        }
        break;
      }
      case 'doc:closed': {
        if (s.screen === 'editor') void s.closeProject();
        break;
      }
    }
  },

  async applyOps(ops, groupLabel) {
    if (ops.length === 0) return;
    try {
      const res = await window.cutboard.applyOps(ops, groupLabel);
      if (res.ok && res.doc) {
        set({ doc: res.doc as TimelineDoc });
      }
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      get().showToast(message.slice(0, 300), { kind: 'error' });
    }
  },

  async undo() {
    const res = await window.cutboard.undo();
    if (res.ok && res.doc) set({ doc: res.doc as TimelineDoc });
    else if (!res.ok) get().showToast('Nothing to undo');
  },

  async redo() {
    const res = await window.cutboard.redo();
    if (res.ok && res.doc) set({ doc: res.doc as TimelineDoc });
    else if (!res.ok) get().showToast('Nothing to redo');
  },

  setPlayhead(frame) {
    set({ playhead: Math.max(0, Math.round(frame)) });
    pushEditorContext();
  },

  stepFrames(n) {
    get().setPlayhead(get().playhead + n);
  },

  setPlaying(playing) {
    set({ playing });
  },

  togglePlay() {
    set({ playing: !get().playing });
  },

  setPxPerFrame(px) {
    set({ pxPerFrame: Math.min(40, Math.max(0.2, px)) });
  },

  zoomFit(viewportFrames) {
    // measure the real lane viewport (minus the gutter and a little breathing room)
    const el = typeof document !== 'undefined' ? document.querySelector<HTMLElement>('[data-timeline-viewport]') : null;
    const laneWidth = el ? el.clientWidth - HEADER_W - 24 : window.innerWidth - 480;
    const width = Math.max(120, laneWidth);
    set({ pxPerFrame: Math.min(40, Math.max(0.2, width / Math.max(1, viewportFrames))), fitNonce: get().fitNonce + 1 });
  },

  toggleSnap() {
    set({ snapEnabled: !get().snapEnabled });
  },

  toggleRipple() {
    set({ rippleEnabled: !get().rippleEnabled });
  },

  select(itemId, additive) {
    if (itemId === null) set({ selection: [] });
    else if (additive) {
      const sel = get().selection;
      set({ selection: sel.includes(itemId) ? sel.filter((x) => x !== itemId) : [...sel, itemId] });
    } else set({ selection: [itemId] });
    pushEditorContext();
  },

  setSelection(ids) {
    set({ selection: ids });
    pushEditorContext();
  },

  async deleteSelection() {
    const { selection, rippleEnabled, doc } = get();
    if (!doc || selection.length === 0) return;
    await get().applyOps(
      [{ type: 'item.remove', itemIds: selection, ripple: rippleEnabled }],
      rippleEnabled ? 'Ripple delete' : 'Delete',
    );
    const removed = selection.filter((id) => !get().doc?.items.some((i) => i.id === id)).length;
    set({ selection: [] });
    if (removed > 0) {
      get().showToast(`Deleted ${removed} clip${removed === 1 ? '' : 's'}`, { action: { label: 'Undo', run: () => void get().undo() } });
    }
  },

  async splitAtPlayhead() {
    const { doc, playhead, selection } = get();
    if (!doc) return;
    const targets = doc.items.filter((i) => {
      const inside = playhead > i.startFrame && playhead < i.startFrame + i.durationFrames;
      if (selection.length > 0) return selection.includes(i.id) && inside;
      return inside;
    });
    if (targets.length === 0) {
      get().showToast('Nothing to split at the playhead.');
      return;
    }
    const newIds = targets.map(() => newId('itm'));
    const ops: Op[] = targets.map((i, idx) => ({
      type: 'item.split',
      itemId: i.id,
      atFrame: playhead,
      newItemId: newIds[idx]!,
    }));
    await get().applyOps(ops, 'Split');
    set({ selection: newIds });
  },

  async cloneSelection() {
    const { doc, selection } = get();
    if (!doc || selection.length === 0) return;
    const ops: Op[] = selection.map((id) => ({
      type: 'item.clone',
      itemId: id,
      newItemId: newId('itm'),
    }));
    await get().applyOps(ops, 'Clone');
  },

  async addAssetToTimeline(assetId, atFrame, trackId) {
    const { doc, assets, snapEnabled, playhead } = get();
    if (!doc) return;
    const asset = assets.find((a) => a.id === assetId);
    if (!asset) return;
    const fps = doc.project.fps;
    const durationFrames =
      asset.kind === 'image'
        ? Math.round(fps * 5) // 5s default for stills
        : Math.max(1, Math.round((asset.durationMs / 1000) * fps));
    const itemType: ItemType = asset.kind === 'audio' ? 'audio' : asset.kind === 'image' ? 'image' : 'video';
    const track =
      (trackId && doc.tracks.find((t) => t.id === trackId)) ??
      doc.tracks.find((t) => t.kind === (itemType === 'audio' ? 'audio' : 'video'));
    if (!track) {
      get().showToast('No compatible track.');
      return;
    }
    const startFrame = Math.max(0, atFrame ?? playhead);
    // snap the prospective clip's in/out edges against existing edges + markers
    let snapped = startFrame;
    if (snapEnabled) {
      const threshold = Math.round(8 / get().pxPerFrame);
      const candidates = getSnapCandidates(doc, { includeItemEdges: true, includeMarkers: true });
      const inSnap = snapFrame(startFrame, candidates, threshold);
      const outSnap = snapFrame(startFrame + durationFrames, candidates, threshold);
      if (inSnap && (!outSnap || Math.abs(inSnap.delta) <= Math.abs(outSnap.delta))) snapped = inSnap.frame;
      else if (outSnap) snapped = outSnap.frame - durationFrames;
    }
    const item = createItem(itemType, {
      id: newId('itm'),
      trackId: track.id,
      startFrame: Math.max(0, snapped),
      durationFrames,
      assetId: asset.id,
      sourceInFrame: 0,
      labels: { name: asset.originalName },
    } as never);
    await get().applyOps([{ type: 'item.add', item }], 'Add clip');
    set({ selection: [item.id] });
  },

  setExportDialog(open) {
    set({ exportDialogOpen: open });
  },

  showToast(message, opts = {}) {
    const id = ++toastSeq;
    const kind = opts.kind ?? 'info';
    // replace an identical message instead of stacking duplicates; keep at most 3 on screen
    const rest = get().toasts.filter((t) => t.message !== message).slice(-2);
    set({ toasts: [...rest, { id, kind, message, action: opts.action }] });
    // errors and actionable toasts stay longer; long messages get time to be read
    const ms = opts.durationMs ?? Math.max(kind === 'error' || opts.action ? 7000 : 3500, message.length * 45);
    setTimeout(() => get().dismissToast(id), ms);
  },

  dismissToast(id) {
    set({ toasts: get().toasts.filter((t) => t.id !== id) });
  },
}));

export function itemName(item: Item): string {
  // a text item's label is a snapshot from creation; its current text is what the user recognises
  const text = (item.props as { text?: unknown } | undefined)?.text;
  if (item.type === 'text' && typeof text === 'string' && text.trim()) return text.trim().slice(0, 60);
  return item.labels?.name ?? `${item.type} ${item.id.slice(4, 10)}`;
}

/** Keep the main process informed for agent tools ("the selected clip", "at this point"). */
function pushEditorContext(): void {
  const s = useEditor.getState();
  try {
    window.cutboard.setEditorContext({
      selection: s.selection,
      playheadFrame: Math.round(s.playhead),
      openProjectId: s.doc?.project.id,
    });
  } catch {
    /* bridge not ready yet */
  }
}
