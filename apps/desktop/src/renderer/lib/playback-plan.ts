import type { Item, TimelineDoc } from '@cutboard/schema';
import { isAudioBearing, itemEnd } from '@cutboard/editor-core';

/**
 * Preview playback keeps ONE media element per asset, but a timeline can hold many items
 * of the same asset (every split makes another). These helpers decide, per asset, which
 * single item an element should currently follow - or null when it must be silent/paused -
 * so one item's "inactive" state can never pause the element another item is playing.
 */

const isActive = (item: Item, frame: number) => frame >= item.startFrame && frame < itemEnd(item);

/** assetId -> the item an audio element should follow at `frame` (null = pause it). */
export function planAudio(doc: TimelineDoc, frame: number): Map<string, Item | null> {
  const plan = new Map<string, Item | null>();
  for (const item of doc.items) {
    if (!isAudioBearing(item) || !item.assetId) continue;
    if (!plan.has(item.assetId)) plan.set(item.assetId, null);
    if (plan.get(item.assetId) || item.muted || !isActive(item, frame)) continue;
    if (doc.tracks.find((t) => t.id === item.trackId)?.muted) continue;
    plan.set(item.assetId, item);
  }
  return plan;
}

/** assetId -> the video item a video element should follow at `frame` (null = pause it). */
export function planVideo(doc: TimelineDoc, frame: number): Map<string, Item | null> {
  const plan = new Map<string, Item | null>();
  for (const item of doc.items) {
    if (item.type !== 'video' || !item.assetId) continue;
    if (!plan.has(item.assetId)) plan.set(item.assetId, null);
    if (plan.get(item.assetId) || !isActive(item, frame)) continue;
    if (doc.tracks.find((t) => t.id === item.trackId)?.hidden) continue;
    plan.set(item.assetId, item);
  }
  return plan;
}

/** True when an element's clock has drifted from where its item says it should be. */
export function needsResync(elementTimeSec: number, expectedSec: number, toleranceSec = 0.3): boolean {
  return Math.abs(elementTimeSec - expectedSec) > toleranceSec;
}

/**
 * Run an async job at most one at a time, always finishing with the most recently
 * requested argument. Used for canvas draws so overlapping async draws cannot interleave
 * (each draw resets the canvas) or finish out of order and leave a stale frame.
 */
export function latestOnly<A>(job: (arg: A) => Promise<void>): (arg: A) => Promise<void> {
  let running = false;
  let queued: { arg: A } | null = null;
  return async (arg: A) => {
    if (running) {
      queued = { arg };
      return;
    }
    running = true;
    try {
      let next: { arg: A } | null = { arg };
      while (next) {
        queued = null;
        await job(next.arg);
        next = queued;
      }
    } finally {
      running = false;
    }
  };
}
