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
import { assetsToMap } from './lib/media.ts';

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
  recentProjects: { id: string; name: string; updatedAt: string }[];

  doc: TimelineDoc | null;
  assets: Asset[];
  jobs: Record<string, JobInfo>;
  exportsList: ExportRowInfo[];
  mediaPool: MediaPool | null;

  selection: string[];
  playhead: number;
  playing: boolean;
  pxPerFrame: number;
  snapEnabled: boolean;
  rippleEnabled: boolean;
  exportDialogOpen: boolean;
  toast: string | null;

  bootstrap(): Promise<void>;
  refreshRecents(): Promise<void>;
  createProject(name?: string): Promise<void>;
  openProject(id: string): Promise<void>;
  closeProject(): Promise<void>;
  revealProjectDir(): Promise<void>;
  importMedia(): Promise<void>;
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
  deleteSelection(): Promise<void>;
  splitAtPlayhead(): Promise<void>;
  cloneSelection(): Promise<void>;
  addAssetToTimeline(assetId: string, atFrame?: number, trackId?: string): Promise<void>;
  setExportDialog(open: boolean): void;
  showToast(message: string): void;
}

const DEFAULT_PX_PER_FRAME = 3;

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
  snapEnabled: true,
  rippleEnabled: false,
  exportDialogOpen: false,
  toast: null,

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

  async createProject(name) {
    const project = await window.cutboard.createProject({ name: name ?? 'Untitled project' });
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
    const jobsList = (await window.cutboard.listJobs(id)) as JobInfo[];
    set({ jobs: Object.fromEntries(jobsList.map((j) => [j.id, j])) });
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
    const imported = (await window.cutboard.pickMediaFiles()) as Asset[];
    if (imported.length > 0) {
      // asset events for these files can arrive before this call returns; merge by id so
      // the panel doesn't show a second, never-updated copy
      const byId = new Map(get().assets.map((a) => [a.id, a]));
      for (const a of imported) if (!byId.has(a.id)) byId.set(a.id, a);
      set({ assets: [...byId.values()] });
      get().showToast(`Importing ${imported.length} file${imported.length > 1 ? 's' : ''}…`);
    }
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
      case 'asset': {
        const asset = envelope.payload as Asset;
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
      get().showToast(message.slice(0, 200));
    }
  },

  async undo() {
    const res = await window.cutboard.undo();
    if (res.ok && res.doc) set({ doc: res.doc as TimelineDoc });
  },

  async redo() {
    const res = await window.cutboard.redo();
    if (res.ok && res.doc) set({ doc: res.doc as TimelineDoc });
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
    const width = Math.max(200, window.innerWidth - 480);
    set({ pxPerFrame: Math.min(40, Math.max(0.2, (width * 0.9) / viewportFrames)) });
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

  async deleteSelection() {
    const { selection, rippleEnabled, doc } = get();
    if (!doc || selection.length === 0) return;
    await get().applyOps(
      [{ type: 'item.remove', itemIds: selection, ripple: rippleEnabled }],
      rippleEnabled ? 'Ripple delete' : 'Delete',
    );
    set({ selection: [] });
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

  showToast(message) {
    set({ toast: message });
    setTimeout(() => {
      if (get().toast === message) set({ toast: null });
    }, 3500);
  },
}));

export function itemName(item: Item): string {
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
