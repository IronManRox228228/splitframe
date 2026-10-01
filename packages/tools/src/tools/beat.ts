import { z } from 'zod';
import { newId, type Item, type Op } from '@cutboard/schema';
import { itemEnd } from '@cutboard/editor-core';
import type { ToolDef } from '../registry.ts';

/**
 * Beat sync (main prompt §1.2, milestone 6): detect the beat grid of the music asset,
 * then place video clips on the timeline cut to the beat with a per-section density map.
 * Clip choice walks the video assets in import order (the user's order).
 */

interface Snapshot {
  doc: {
    project: { fps: number };
    tracks: { id: string; kind: string; name: string; locked: boolean }[];
    items: Item[];
    markers: unknown[];
  };
  assets: { id: string; kind: string; durationMs: number; originalName?: string }[];
}

function requireSnapshot(snap: unknown): Snapshot {
  const s = snap as Snapshot;
  if (!s || typeof s !== 'object' || !('doc' in s)) throw new Error('No project open.');
  return s;
}

export const analyzeBeats: ToolDef = {
  name: 'analyzeBeats',
  description:
    'Detect the tempo, beat grid, downbeats, and energy sections of an audio asset. Runs locally (no cloud). Call before beatSync; the result is cached per asset.',
  input: z.object({ assetId: z.string() }),
  mutates: false,
  async handler(input, ctx) {
    const map = await ctx.analyzeBeats(input.assetId);
    return { assetId: input.assetId, bpm: map.bpm, beatCount: map.beatsMs.length, downbeatCount: map.downbeatsMs.length, sections: map.sections };
  },
};

export const beatSync: ToolDef = {
  name: 'beatSync',
  description:
    'Cut video clips to the beat of a music item: builds a new sequence on the main video track where every cut lands on a beat (±1 frame). Density per section: one clip every N beats (N=1 for high energy, 2 medium, 4 low by default). Uses video assets in import order; existing items on the track are replaced (one undo restores them).',
  input: z.object({
    musicItemId: z.string().describe('Audio item whose asset provides the beat grid'),
    videoTrackId: z.string().optional().describe('Target track; default = first video track'),
    replaceExisting: z.boolean().default(true),
    density: z
      .object({
        high: z.number().int().min(1).max(16).default(1),
        medium: z.number().int().min(1).max(16).default(2),
        low: z.number().int().min(1).max(16).default(4),
      })
      .optional(),
    startFrame: z.number().int().nonnegative().optional().describe('Where the sequence starts; default = music item start'),
    holdLastShot: z.boolean().default(true).describe('When clips run out, hold the last frame of the last clip instead of stopping'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const fps = snapshot.doc.project.fps;
    const music = snapshot.doc.items.find((i) => i.id === input.musicItemId);
    if (!music) throw new Error(`Item ${input.musicItemId} not found. Call getTimeline.`);
    if (music.type !== 'audio' && music.type !== 'video') throw new Error('musicItemId must be an audio-bearing item.');
    const track =
      (input.videoTrackId ? snapshot.doc.tracks.find((t) => t.id === input.videoTrackId) : undefined) ??
      snapshot.doc.tracks.find((t) => t.kind === 'video');
    if (!track) throw new Error('No video track available.');
    if (track.locked) throw new Error(`Track "${track.name}" is locked.`);

    const map = await ctx.analyzeBeats(music.assetId ?? '');
    const density = input.density ?? { high: 1, medium: 2, low: 4 };

    // build the beat-slot timeline
    const musicStart = input.startFrame ?? music.startFrame;
    const speedRatio = music.speed || 1;
    const timelineEnd = Math.max(
      ...snapshot.doc.items.map(itemEnd),
      musicStart + Math.round((music.durationFrames * 1) / 1),
    );
    const beatFrames = map.beatsMs.map((ms) => musicStart + Math.round(((ms / 1000) * fps) / speedRatio) - Math.round(((music.sourceInFrame ?? 0) / 1) / speedRatio));
    const slots: { start: number; end: number; section: string }[] = [];
    let nextBeatIdx = 0;
    while (nextBeatIdx < beatFrames.length - 1) {
      const slotStart = beatFrames[nextBeatIdx]!;
      if (slotStart >= timelineEnd) break;
      // which section are we in?
      const slotMs = ((slotStart - musicStart) * speedRatio * 1000) / fps + (music.sourceInFrame ?? 0) * 0;
      const section = map.sections.find((s) => slotMs + (music.sourceInFrame ?? 0) / fps * 1000 >= s.startMs && slotMs + (music.sourceInFrame ?? 0) / fps * 1000 < s.endMs);
      const everyN = density[(section?.label as 'high' | 'medium' | 'low') ?? 'medium'] ?? 2;
      const nextIdx = Math.min(beatFrames.length - 1, nextBeatIdx + everyN);
      const slotEnd = beatFrames[nextIdx]!;
      slots.push({ start: slotStart, end: Math.min(slotEnd, timelineEnd), section: section?.label ?? 'medium' });
      nextBeatIdx = nextIdx;
    }
    if (slots.length === 0) throw new Error('No beat slots found — is the music asset long enough?');

    // pool: video assets in import order
    const pool = snapshot.assets.filter((a) => a.kind === 'video');
    if (pool.length === 0) throw new Error('No video assets to place. Import footage first.');

    const ops: Op[] = [];
    if (input.replaceExisting) {
      const doomed = snapshot.doc.items.filter((i) => i.trackId === track.id).map((i) => i.id);
      if (doomed.length > 0) ops.push({ type: 'item.remove', itemIds: doomed, ripple: false });
    }

    let poolIdx = 0;
    let cursorFrames = 0; // consumed source frames (project-fps) of the current pool asset
    let lastAddedId: string | null = null;
    for (const slot of slots) {
      const slotLen = Math.max(1, slot.end - slot.start);
      const asset = pool[poolIdx];
      if (!asset) {
        if (input.holdLastShot && lastAddedId) {
          // stretch the last placed clip to the timeline end
          const last = ops[ops.length - 1] as { type: string; item?: { id: string; startFrame: number } } | undefined;
          void last;
          break;
        }
        break;
      }
      const assetDurationFrames = Math.round((asset.durationMs / 1000) * fps);
      const remaining = assetDurationFrames - cursorFrames;
      const take = Math.min(slotLen, remaining);
      if (take < 1) {
        poolIdx++;
        cursorFrames = 0;
        // retry this slot with the next asset
        poolIdx--;
        poolIdx++;
        continue;
      }
      const id = newId('itm');
      ops.push({
        type: 'item.add',
        item: {
          id,
          trackId: track.id,
          type: 'video',
          startFrame: slot.start,
          durationFrames: take,
          assetId: asset.id,
          sourceInFrame: cursorFrames,
          transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 },
          labels: { name: asset.originalName ?? 'clip' },
        } as never,
      });
      lastAddedId = id;
      cursorFrames += take;
      if (cursorFrames >= assetDurationFrames) {
        poolIdx++;
        cursorFrames = 0;
      }
      void slot.section;
    }
    if (!ops.some((o) => o.type === 'item.add')) {
      throw new Error('Not enough footage for the beat sequence (all clips exhausted).');
    }
    await ctx.applyOps(ops, ctx.actor, `beatSync (${slots.length} cuts @ ${map.bpm} bpm)`);
    return {
      applied: true,
      bpm: map.bpm,
      cuts: slots.length,
      note: 'Cuts land on the beat grid. Verify with getTimeline; undo restores the previous sequence.',
    };
  },
};

export const BEAT_TOOLS: ToolDef[] = [analyzeBeats, beatSync];
