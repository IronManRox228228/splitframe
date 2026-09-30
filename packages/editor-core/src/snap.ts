import { TimelineDoc } from '@cutboard/schema';
import { itemEnd, itemsOnTrack } from './timeline-doc.ts';

export interface SnapCandidate {
  frame: number;
  kind: 'item-edge' | 'marker' | 'playhead' | 'grid' | 'zero';
  source?: string;
}

export interface SnapOptions {
  includeItemEdges?: boolean;
  includeMarkers?: boolean;
  /** the live playhead position (a frame number) as an extra candidate */
  includePlayhead?: number;
  gridFrames?: number;
  excludeItemIds?: string[];
  /** extra candidates supplied by the UI (e.g. the live playhead position) */
  extra?: SnapCandidate[];
}

export function getSnapCandidates(doc: TimelineDoc, opts: SnapOptions = {}): SnapCandidate[] {
  const out: SnapCandidate[] = [{ frame: 0, kind: 'zero' }];
  if (opts.includeItemEdges !== false) {
    for (const track of doc.tracks) {
      if (track.kind === 'audio') continue; // visual edges are what you snap against
      for (const item of itemsOnTrack(doc, track.id)) {
        if (opts.excludeItemIds?.includes(item.id)) continue;
        out.push({ frame: item.startFrame, kind: 'item-edge', source: item.id });
        out.push({ frame: itemEnd(item), kind: 'item-edge', source: item.id });
      }
    }
  }
  if (opts.includeMarkers !== false) {
    for (const m of doc.markers) out.push({ frame: m.frame, kind: 'marker', source: m.id });
  }
  if (opts.includePlayhead) out.push({ frame: opts.includePlayhead, kind: 'playhead' });
  if (opts.extra) out.push(...opts.extra);
  if (opts.gridFrames && opts.gridFrames > 1) {
    const end = Math.max(docDuration(doc), 1) + opts.gridFrames;
    for (let f = 0; f <= end; f += opts.gridFrames) out.push({ frame: f, kind: 'grid' });
  }
  return out;
}

function docDuration(doc: TimelineDoc): number {
  return doc.items.reduce((max, i) => Math.max(max, itemEnd(i)), 0);
}

export interface SnapResult {
  frame: number;
  candidate: SnapCandidate;
  /** distance in frames from the original position */
  delta: number;
}

/** Snap `frame` to the nearest candidate within `thresholdFrames`. */
export function snapFrame(
  frame: number,
  candidates: SnapCandidate[],
  thresholdFrames: number,
): SnapResult | null {
  let best: SnapResult | null = null;
  for (const candidate of candidates) {
    const delta = candidate.frame - frame;
    if (Math.abs(delta) > thresholdFrames) continue;
    if (!best || Math.abs(delta) < Math.abs(best.delta)) {
      best = { frame: candidate.frame, candidate, delta };
    }
  }
  return best;
}

/**
 * Snap a moving item so its in OR out edge lands on a candidate (whichever is closer).
 * Returns the adjusted start frame.
 */
export function snapItemStart(
  doc: TimelineDoc,
  itemId: string,
  proposedStart: number,
  thresholdFrames: number,
  opts: SnapOptions = {},
): number {
  const item = doc.items.find((i) => i.id === itemId);
  if (!item) return proposedStart;
  const candidates = getSnapCandidates(doc, {
    ...opts,
    excludeItemIds: [...(opts.excludeItemIds ?? []), itemId],
  });
  const inSnap = snapFrame(proposedStart, candidates, thresholdFrames);
  const outSnap = snapFrame(proposedStart + item.durationFrames, candidates, thresholdFrames);
  if (inSnap && (!outSnap || Math.abs(inSnap.delta) <= Math.abs(outSnap.delta))) {
    return Math.max(0, inSnap.frame);
  }
  if (outSnap) return Math.max(0, outSnap.frame - item.durationFrames);
  return proposedStart;
}
