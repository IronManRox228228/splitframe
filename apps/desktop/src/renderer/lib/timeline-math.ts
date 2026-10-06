import type { Op } from '@cutboard/schema';

/** Width of the sticky track-header column that precedes every lane. */
export const HEADER_W = 168;

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
}

/**
 * Timeline frame under a pointer. `rowLeft` is the left edge of a track row (it includes
 * the header column), so the header width is removed before converting pixels to frames.
 */
export function frameFromPointer(clientX: number, rowLeft: number, pxPerFrame: number): number {
  return Math.max(0, Math.round((clientX - rowLeft - HEADER_W) / pxPerFrame));
}

/** The op a finished drag should commit, or null when nothing changed. */
export function dragCommit(drag: DragState, ripple: boolean): { ops: Op[]; label: string } | null {
  if (drag.kind === 'move') {
    const trackChanged = drag.ghostTrackId !== drag.origTrackId;
    const startChanged = drag.ghostStart !== drag.origStart;
    if (!trackChanged && !startChanged) return null;
    return {
      ops: [
        {
          type: 'item.move',
          itemId: drag.itemId,
          ...(trackChanged ? { trackId: drag.ghostTrackId } : {}),
          ...(startChanged ? { startFrame: drag.ghostStart } : {}),
        },
      ],
      label: 'Move clip',
    };
  }
  if (drag.ghostFrame === undefined) return null;
  const edge = drag.kind === 'trim-in' ? 'in' : 'out';
  return {
    ops: [{ type: 'item.trim', itemId: drag.itemId, edge, frame: drag.ghostFrame, ripple }],
    label: edge === 'in' ? 'Trim in' : 'Trim out',
  };
}
