import type { Task } from '../types.ts';
import { check, finish, fmt, closeness, guard } from '../lib/score.ts';
import { docDurationSec, endFrame, itemsOfAsset, itemsOfType, keptSource, trackLayout } from '../lib/timeline.ts';

/** Clip-level edits on the b-roll: trim, reorder, delete the selection, beat cuts. */

const order = (input: Parameters<Task['check']>[0]): string[] => {
  const names: Record<string, string> = {};
  for (const [name, id] of Object.entries(input.assetIds)) names[id] = name.replace('broll-', '').replace('.mp4', '').toUpperCase();
  return itemsOfType(input.doc, 'video').map((i) => names[i.assetId ?? ''] ?? '?');
};

export const trimClip: Task = {
  id: 'trim-clip',
  title: 'Trim a clip to its first 4 seconds',
  difficulty: 'easy',
  fixtures: ['broll-c.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-c.mp4');
  },
  messages: ['Trim broll-c so that only its first 4 seconds play.'],
  check(input) {
    const items = itemsOfType(input.doc, 'video');
    const item = items[0];
    if (!item) return finish([check('clip still on the timeline', false, 'no video items', 1)]);
    const fps = input.fps;
    const src = keptSource(input.doc, input.assetIds['broll-c.mp4']);
    return finish([
      guard('clip still on the timeline', items.length === 1, `${items.length} video item(s)`, 1),
      check('plays 4 seconds', closeness(item.durationFrames / fps, 4, 0.12, 2), `lasts ${fmt(item.durationFrames / fps)}s`, 4),
      guard('plays the FIRST 4 seconds of the source', closeness(src[0]?.start ?? 99, 0, 0.1, 1), `source starts at ${fmt(src[0]?.start ?? -1)}s`, 2),
      guard('stays at the start of the timeline', item.startFrame <= 3, `starts at frame ${item.startFrame}`, 1),
    ]);
  },
};

export const reorder: Task = {
  id: 'reorder-clips',
  title: 'Put b-roll C before b-roll A',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: async (ctx) => {
    for (const f of ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'] as const) await ctx.addToTimeline(f);
  },
  messages: ['Move broll-c so it plays first, before broll-a. Keep everything else in the same order and with no gaps or overlaps.'],
  check(input) {
    const o = order(input);
    const items = itemsOfType(input.doc, 'video');
    const layout = trackLayout(items, input.fps);
    const fullLen = (name: 'broll-a.mp4' | 'broll-b.mp4' | 'broll-c.mp4') => input.manifest.fixtures[name].durationSec;
    const total = fullLen('broll-a.mp4') + fullLen('broll-b.mp4') + fullLen('broll-c.mp4');
    return finish([
      check('C plays before A', o.indexOf('C') >= 0 && o.indexOf('C') < o.indexOf('A'), `order ${o.join(' > ')}`, 3),
      check('final order is C, A, B', o.join('') === 'CAB', `order ${o.join(' > ')}`, 2),
      guard('all three clips intact', items.length === 3 && closeness(docDurationSec(input.doc), total, 0.3, 3) >= 0.99, `${items.length} items, ${fmt(docDurationSec(input.doc))}s (want ${total}s)`, 2),
      guard('no overlaps', layout.overlapSec <= 0.05, `overlap ${fmt(layout.overlapSec)}s`, 1),
      guard('no gaps', layout.maxGapSec <= 0.1, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

export const deleteSelected: Task = {
  id: 'delete-selected',
  title: 'Delete the selected clip (editor context)',
  difficulty: 'easy',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
    const b = await ctx.addToTimeline('broll-b.mp4');
    await ctx.addToTimeline('broll-c.mp4');
    ctx.select([b]);
  },
  messages: ['Delete the selected clip.'],
  check(input) {
    const b = input.setupItems['broll-b.mp4'];
    const ids = new Set(input.doc.items.map((i) => i.id));
    const aKept = itemsOfAsset(input.doc, input.assetIds['broll-a.mp4']).length > 0;
    const cKept = itemsOfAsset(input.doc, input.assetIds['broll-c.mp4']).length > 0;
    return finish([
      check('selected clip (B) deleted', b !== undefined && !ids.has(b) && itemsOfAsset(input.doc, input.assetIds['broll-b.mp4']).length === 0, `B ${b && ids.has(b) ? 'still present' : 'gone'}`, 4),
      guard('clip A untouched', aKept, aKept ? 'present' : 'missing', 2),
      guard('clip C untouched', cKept, cKept ? 'present' : 'missing', 2),
      guard('nothing else removed', input.doc.items.length === 2, `${input.doc.items.length} items left (want 2)`, 1),
    ]);
  },
};

export const beatCuts: Task = {
  id: 'cut-to-beat',
  title: 'Cut the three b-roll clips to the beat of the music',
  difficulty: 'hard',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4', 'music.mp3'],
  setup: async (ctx) => {
    await ctx.addToTimeline('music.mp3');
  },
  messages: ['Cut my three b-roll clips (broll-a, broll-b, broll-c) to the beat of the music track.'],
  check(input) {
    const music = input.manifest.fixtures['music.mp3'];
    const fps = input.fps;
    const videos = itemsOfType(input.doc, 'video');
    const used = ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'].filter((n) => itemsOfAsset(input.doc, input.assetIds[n]).some((i) => i.type === 'video'));
    const cuts = videos.slice(1).map((i) => i.startFrame / fps);
    const musicItem = itemsOfAsset(input.doc, input.assetIds['music.mp3'])[0];
    const origin = (musicItem?.startFrame ?? 0) / fps + music.firstBeatSec;
    // distance of each cut to the nearest beat (tolerance 3 frames = 0.1 s)
    const offBeat = cuts.map((c) => {
      const k = Math.round((c - origin) / music.beatPeriodSec);
      return Math.abs(c - (origin + k * music.beatPeriodSec));
    });
    const onBeat = offBeat.filter((d) => d <= 0.1).length;
    const layout = trackLayout(videos, fps);
    return finish([
      check('all three clips used', used.length / 3, `${used.length}/3 b-roll assets on the video track`, 2),
      check('there are cuts', cuts.length >= 2, `${videos.length} video items, ${cuts.length} cut points`, 2),
      check('cuts land on the beat', cuts.length ? onBeat / cuts.length : 0, `${onBeat}/${cuts.length} cuts within 0.1s of a beat (${music.bpm} BPM grid)`, 4),
      guard('music still on the timeline', musicItem !== undefined, musicItem ? 'present' : 'missing', 1),
      guard('no overlapping video', layout.overlapSec <= 0.05, `overlap ${fmt(layout.overlapSec)}s`, 1),
      check('video fits the music', videos.length > 0 && Math.max(...videos.map(endFrame)) / fps <= music.durationSec + 0.5, `video ends at ${fmt(Math.max(0, ...videos.map(endFrame)) / fps)}s`, 1),
    ]);
  },
};
