import type { Op } from '@cutboard/schema';

/** Width of the sticky icon gutter that precedes every lane. */
export const HEADER_W = 44;

/** Vertical gap kept above and below a clip inside its row. */
export const ROW_PAD = 4;

export interface DragState {
  kind: 'move' | 'trim-in' | 'trim-out';
  itemId: string;
  pointerId: number;
  grabOffsetFrames: number;
  origStart: number;
  origTrackId: string;
  ghostStart: number;
  ghostTrackId: string;
  ghostFrame?: number;
  /** other selected items that move by the same delta as the primary one (move only) */
  groupIds?: string[];
  groupOrigStarts?: Record<string, number>;
  /** frame a snap landed on, drawn as a vertical guide */
  guideFrame?: number | null;
  /** lane the pointer is currently over (for the not-allowed cursor) */
  hoverTrackId?: string;
}

/**
 * Timeline frame under a pointer. `rowLeft` is the left edge of a track row (it includes
 * the gutter column), so the gutter width is removed before converting pixels to frames.
 */
export function frameFromPointer(clientX: number, rowLeft: number, pxPerFrame: number): number {
  return Math.max(0, Math.round((clientX - rowLeft - HEADER_W) / pxPerFrame));
}

/** The ops a finished drag should commit, or null when nothing changed. */
export function dragCommit(drag: DragState, ripple: boolean): { ops: Op[]; label: string } | null {
  if (drag.kind === 'move') {
    const trackChanged = drag.ghostTrackId !== drag.origTrackId;
    const startChanged = drag.ghostStart !== drag.origStart;
    if (!trackChanged && !startChanged) return null;
    const ops: Op[] = [
      {
        type: 'item.move',
        itemId: drag.itemId,
        ...(trackChanged ? { trackId: drag.ghostTrackId } : {}),
        ...(startChanged ? { startFrame: drag.ghostStart } : {}),
      },
    ];
    const others = drag.groupIds?.filter((id) => id !== drag.itemId) ?? [];
    if (others.length > 0 && startChanged) {
      const delta = drag.ghostStart - drag.origStart;
      for (const id of others) {
        const orig = drag.groupOrigStarts?.[id];
        if (orig === undefined) continue;
        ops.push({ type: 'item.move', itemId: id, startFrame: Math.max(0, orig + delta) });
      }
    }
    return { ops, label: ops.length > 1 ? 'Move clips' : 'Move clip' };
  }
  if (drag.ghostFrame === undefined) return null;
  const edge = drag.kind === 'trim-in' ? 'in' : 'out';
  return {
    ops: [{ type: 'item.trim', itemId: drag.itemId, edge, frame: drag.ghostFrame, ripple }],
    label: edge === 'in' ? 'Trim in' : 'Trim out',
  };
}

/** Row height (clip height plus padding) per track kind. */
export function rowHeightForKind(kind: string): number {
  if (kind === 'video') return 64;
  if (kind === 'audio') return 48;
  return 44;
}

/** Cumulative top offset and height of each row. */
export function trackLayout(kinds: string[]): { top: number; height: number }[] {
  let top = 0;
  return kinds.map((k) => {
    const height = rowHeightForKind(k);
    const row = { top, height };
    top += height;
    return row;
  });
}

/** Index of the row at a y offset inside the lanes (clamped to the first/last row). */
export function rowIndexAtY(layout: { top: number; height: number }[], y: number): number {
  if (layout.length === 0) return -1;
  for (let i = 0; i < layout.length; i++) {
    const r = layout[i]!;
    if (y < r.top + r.height) return i;
  }
  return layout.length - 1;
}

const SECOND_STEPS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600];

/**
 * Major ruler tick interval in frames: the smallest "nice" interval (sub-second frame
 * counts, then whole seconds/minutes) that keeps labels at least `minPx` apart.
 */
export function rulerInterval(pxPerFrame: number, fps: number, minPx = 88): number {
  const subSecond = [1, 2, 5, 10].filter((f) => f < fps);
  const candidates = [...subSecond, ...SECOND_STEPS.map((s) => Math.round(s * fps))];
  return candidates.find((f) => f * pxPerFrame >= minPx) ?? candidates[candidates.length - 1]!;
}

/** Minor ticks per major interval (0 when they would be crowded). */
export function rulerSubdivisions(interval: number, pxPerFrame: number): number {
  for (const n of [5, 4, 2]) {
    if (interval % n === 0 && (interval / n) * pxPerFrame >= 8) return n;
  }
  return 0;
}

/** Hit width in px of a trim handle: ~8px, at most a third of the clip, none under ~20px. */
export function trimHandleWidth(clipWidthPx: number): number {
  if (clipWidthPx < 20) return 0;
  return Math.min(8, clipWidthPx / 3);
}

/** Mono timecode with frames precision for trim badges, e.g. "1.2s". */
export function formatDurationBadge(frames: number, fps: number): string {
  const secs = Math.max(0, frames) / fps;
  return secs >= 10 ? `${secs.toFixed(1)}s` : `${secs.toFixed(2)}s`;
}

export interface MarqueeItem {
  id: string;
  rowIndex: number;
  startFrame: number;
  endFrame: number;
}

/** Items whose rows and frame range intersect a marquee rectangle (lane-space px). */
export function marqueeHits(
  items: MarqueeItem[],
  layout: { top: number; height: number }[],
  rect: { x1: number; y1: number; x2: number; y2: number },
  pxPerFrame: number,
): string[] {
  const left = Math.min(rect.x1, rect.x2);
  const right = Math.max(rect.x1, rect.x2);
  const top = Math.min(rect.y1, rect.y2);
  const bottom = Math.max(rect.y1, rect.y2);
  const out: string[] = [];
  for (const it of items) {
    const row = layout[it.rowIndex];
    if (!row) continue;
    const rowTop = row.top + ROW_PAD;
    const rowBottom = row.top + row.height - ROW_PAD;
    if (rowBottom < top || rowTop > bottom) continue;
    if (it.endFrame * pxPerFrame < left || it.startFrame * pxPerFrame > right) continue;
    out.push(it.id);
  }
  return out;
}

/** Scroll offset that keeps the content point under `anchorViewX` fixed when zoom changes. */
export function scrollForZoom(anchorFrame: number, anchorViewX: number, newPxPerFrame: number): number {
  return Math.max(0, HEADER_W + anchorFrame * newPxPerFrame - anchorViewX);
}
