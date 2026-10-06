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
  | 'jumpBack'
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
    return null; // everything else (Ctrl+=, Ctrl+-, Ctrl+S ...) belongs to the menu or the OS
  }

  if (e.code === 'Space') return 'togglePlay';
  if (e.key === 'Backspace' || e.key === 'Delete') return 'delete';
  if (e.key === 'ArrowLeft') return 'stepBack';
  if (e.key === 'ArrowRight') return 'stepForward';
  if (e.key === '+' || e.key === '=') return 'zoomIn';
  if (e.key === '-') return 'zoomOut';
  if (e.key === 'Escape') return 'deselect';
  if (key === 's') return 'split';
  if (key === 'j') return 'jumpBack';
  if (key === 'k') return ctx.playing ? 'pause' : null;
  if (key === 'l') return ctx.playing ? null : 'play';
  if (key === 'z' && e.shiftKey) return 'zoomFit';
  return null;
}
