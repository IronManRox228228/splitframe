/**
 * Keyboard shortcut routing for the editor, kept pure so it can be unit-tested.
 *
 * Modifier shortcuts that also exist as menu accelerators (zoom in/out) are NOT handled
 * here: the menu already fires them, and handling them twice zoomed by 1.25x twice.
 */

export type ShortcutAction =
  | 'togglePlay'
  | 'play'
  | 'pause'
  | 'undo'
  | 'redo'
  | 'clone'
  | 'import'
  | 'delete'
  | 'split'
  | 'stepBack'
  | 'stepForward'
  | 'secondBack'
  | 'secondForward'
  | 'goStart'
  | 'goEnd'
  | 'prevEdge'
  | 'nextEdge'
  | 'selectAll'
  | 'toggleSnap'
  | 'zoomIn'
  | 'zoomOut'
  | 'zoomFit'
  | 'deselect';

export interface KeyInfo {
  key: string;
  code: string;
  metaKey: boolean;
  ctrlKey: boolean;
  shiftKey: boolean;
  /** tag name of the focused element */
  targetTag: string;
  isContentEditable: boolean;
}

/** Elements that own the keyboard: typing, caret movement and list selection must not trigger editor shortcuts. */
const TEXT_TARGETS = new Set(['INPUT', 'TEXTAREA', 'SELECT']);

export function shortcutFor(e: KeyInfo, ctx: { playing: boolean }): ShortcutAction | null {
  if (TEXT_TARGETS.has(e.targetTag) || e.isContentEditable) return null;
  const meta = e.metaKey || e.ctrlKey;
  const key = e.key.toLowerCase();

  if (meta) {
    if (key === 'z') return e.shiftKey ? 'redo' : 'undo';
    if (key === 'd') return 'clone';
    if (key === 'i') return 'import';
    if (key === 'a' && !e.shiftKey) return 'selectAll';
    return null; // everything else (Ctrl+=, Ctrl+-, Ctrl+S ...) belongs to the menu or the OS
  }

  if (e.code === 'Space') return 'togglePlay';
  if (e.key === 'Backspace' || e.key === 'Delete') return 'delete';
  if (e.key === 'ArrowLeft') return e.shiftKey ? 'secondBack' : 'stepBack';
  if (e.key === 'ArrowRight') return e.shiftKey ? 'secondForward' : 'stepForward';
  if (e.key === 'Home') return 'goStart';
  if (e.key === 'End') return 'goEnd';
  if (e.key === 'ArrowUp') return 'prevEdge';
  if (e.key === 'ArrowDown') return 'nextEdge';
  if (e.key === '+' || e.key === '=') return 'zoomIn';
  if (e.key === '-') return 'zoomOut';
  if (e.key === 'Escape') return 'deselect';
  if (key === 's') return 'split';
  // the playback engine only runs forward at 1x, so J steps back one second instead of reverse-playing
  if (key === 'j') return 'secondBack';
  if (key === 'k') return ctx.playing ? 'pause' : null;
  if (key === 'l') return ctx.playing ? null : 'play';
  if (key === 'n') return 'toggleSnap';
  if (key === 'z' && e.shiftKey) return 'zoomFit';
  return null;
}

/** Sorted unique clip start/end frames. */
export function clipEdges(items: { startFrame: number; durationFrames: number }[]): number[] {
  const set = new Set<number>();
  for (const i of items) {
    set.add(i.startFrame);
    set.add(i.startFrame + i.durationFrames);
  }
  return [...set].sort((a, b) => a - b);
}

/** Nearest edge strictly before (dir -1) or after (dir 1) the playhead, or null. */
export function neighborEdge(edges: number[], playhead: number, dir: -1 | 1): number | null {
  if (dir === 1) return edges.find((f) => f > playhead) ?? null;
  for (let i = edges.length - 1; i >= 0; i--) if (edges[i]! < playhead) return edges[i]!;
  return null;
}

/** Minimal slice of the editor store the shortcut handler needs (keeps this file testable). */
export interface ShortcutStore {
  doc: { project: { fps: number }; items: { id: string; startFrame: number; durationFrames: number }[] } | null;
  playhead: number;
  playing: boolean;
  pxPerFrame: number;
  togglePlay(): void;
  setPlayhead(frame: number): void;
  stepFrames(n: number): void;
  undo(): Promise<void>;
  redo(): Promise<void>;
  cloneSelection(): Promise<void>;
  importMedia(): Promise<void>;
  deleteSelection(): Promise<void>;
  splitAtPlayhead(): Promise<void>;
  setPxPerFrame(px: number): void;
  zoomFit(frames: number): void;
  toggleSnap(): void;
  select(id: string | null, additive?: boolean): void;
  setSelection(ids: string[]): void;
}

/** Perform a resolved shortcut action against the store. */
export function runShortcut(action: ShortcutAction, s: ShortcutStore): void {
  const fps = s.doc?.project.fps ?? 30;
  const items = s.doc?.items ?? [];
  const total = items.reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);
  switch (action) {
    case 'togglePlay': s.togglePlay(); break;
    case 'play': if (!s.playing) s.togglePlay(); break;
    case 'pause': if (s.playing) s.togglePlay(); break;
    case 'undo': void s.undo(); break;
    case 'redo': void s.redo(); break;
    case 'clone': void s.cloneSelection(); break;
    case 'import': void s.importMedia(); break;
    case 'delete': void s.deleteSelection(); break;
    case 'split': void s.splitAtPlayhead(); break;
    case 'stepBack': s.stepFrames(-1); break;
    case 'stepForward': s.stepFrames(1); break;
    case 'secondBack': s.stepFrames(-Math.round(fps)); break;
    case 'secondForward': s.stepFrames(Math.round(fps)); break;
    case 'goStart': s.setPlayhead(0); break;
    case 'goEnd': s.setPlayhead(total); break;
    case 'prevEdge': {
      const f = neighborEdge(clipEdges(items), s.playhead, -1);
      if (f !== null) s.setPlayhead(f);
      break;
    }
    case 'nextEdge': {
      const f = neighborEdge(clipEdges(items), s.playhead, 1);
      if (f !== null) s.setPlayhead(f);
      break;
    }
    case 'selectAll': s.setSelection(items.map((i) => i.id)); break;
    case 'toggleSnap': s.toggleSnap(); break;
    case 'zoomIn': s.setPxPerFrame(s.pxPerFrame * 1.25); break;
    case 'zoomOut': s.setPxPerFrame(s.pxPerFrame / 1.25); break;
    case 'zoomFit': s.zoomFit(Math.max(1, total)); break;
    case 'deselect': s.select(null); break;
  }
}

/** Keydown entry point: resolve the key to an action and run it. Returns true when handled. */
export function handleKeyDown(e: KeyboardEvent, getState: () => ShortcutStore): boolean {
  const target = e.target as HTMLElement | null;
  const state = getState();
  const action = shortcutFor(
    {
      key: e.key,
      code: e.code,
      metaKey: e.metaKey,
      ctrlKey: e.ctrlKey,
      shiftKey: e.shiftKey,
      targetTag: target?.tagName ?? '',
      isContentEditable: Boolean(target?.isContentEditable),
    },
    { playing: state.playing },
  );
  if (!action) return false;
  e.preventDefault();
  runShortcut(action, state);
  return true;
}
