import type { CheckInput, Task } from '../types.ts';
import { check, closeness, finish, fmt, guard, ramp } from '../lib/score.ts';
import { coveredSec, docDurationSec, itemsOfAsset, itemsOfType, keptFraction, keptSource, mergeIntervals, trackLayout } from '../lib/timeline.ts';
import { pausesRemoved, retakeRemoved, speechKept, titleChecks, wantedSpeech } from '../lib/checks.ts';
import { projectService } from '../../src/main/project-service.ts';

/**
 * Held-out tasks: paraphrases of the core tasks in other words, and requests the harness was never
 * built around. They use the same fixtures and check helpers and are run as a group
 * (`--tasks heldout`); the default suite stays the original 15. Not for tuning: when one fails, fix a
 * general mechanism, never the wording.
 */

const placeInterview: Task['setup'] = async (ctx) => {
  await ctx.addToTimeline('interview.mp4');
};

const brollOrder = (input: CheckInput): string => {
  const names: Record<string, string> = {};
  for (const [name, id] of Object.entries(input.assetIds)) names[id] = name.replace('broll-', '').replace('.mp4', '').toUpperCase();
  return itemsOfType(input.doc, 'video').map((i) => names[i.assetId ?? ''] ?? '?').join('');
};

/** Clips A, B, C back to back in the wanted order, nothing lost, no gaps or overlaps. */
function orderChecks(input: CheckInput, want: string, weight = 3) {
  const items = itemsOfType(input.doc, 'video');
  const layout = trackLayout(items, input.fps);
  const total = (['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'] as const).reduce((a, n) => a + input.manifest.fixtures[n].durationSec, 0);
  const got = brollOrder(input);
  return [
    check(`order is ${want.split('').join(', ')}`, got === want, `order ${got.split('').join(' > ')}`, weight),
    guard('all three clips intact', items.length === 3 && closeness(docDurationSec(input.doc), total, 0.3, 3) >= 0.99, `${items.length} items, ${fmt(docDurationSec(input.doc))}s (want ${total}s)`, 2),
    guard('no overlaps', layout.overlapSec <= 0.05, `overlap ${fmt(layout.overlapSec)}s`, 1),
    guard('no gaps', layout.maxGapSec <= 0.1, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
  ];
}

const placeABC: Task['setup'] = async (ctx) => {
  for (const f of ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'] as const) await ctx.addToTimeline(f);
};

export const tightenDeadAir: Task = {
  id: 'ho-dead-air',
  title: 'Paraphrase: tighten up the dead air',
  difficulty: 'easy',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Tighten up the dead air in the interview.'],
  check(input) {
    const truth = input.manifest.fixtures['interview.mp4'];
    const shrink = docDurationSec(input.initialDoc) - docDurationSec(input.doc);
    return finish([
      pausesRemoved(input, 0.5, 4),
      speechKept(input, wantedSpeech(input, {}), 'speech preserved', 3),
      check('duration shrinks by about the pause total', closeness(shrink, truth.totalPauseSec, 2.5, 6), `shrank ${fmt(shrink)}s, pauses total ${truth.totalPauseSec}s`, 2),
    ]);
  },
};

export const umsAndAhs: Task = {
  id: 'ho-ums-ahs',
  title: 'Paraphrase: get rid of the ums and ahs',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Get rid of the ums and ahs.'],
  check(input) {
    const id = input.assetIds['interview.mp4'];
    const fillers = input.manifest.fixtures['interview.mp4'].fillers.filter((f) => f.word === 'um' || f.word === 'uh');
    const gone = fillers.filter((f) => keptFraction(input.doc, id, { start: f.startSec, end: f.endSec }) <= 0.25);
    return finish([
      check('ums and ahs removed', fillers.length ? gone.length / fillers.length : 1, `${gone.length}/${fillers.length} cut`, 4),
      speechKept(input, wantedSpeech(input, { dropFillers: true }), 'real speech preserved', 3),
    ]);
  },
};

export const flub: Task = {
  id: 'ho-flub',
  title: 'Paraphrase: the first attempt is a flub, drop it',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['The first attempt at the company sentence is a flub, drop it.'],
  check(input) {
    return finish([...retakeRemoved(input, 3), speechKept(input, wantedSpeech(input, { dropRetake: true }), 'rest of the speech preserved', 2)]);
  },
};

export const shortenC: Task = {
  id: 'ho-shorten-c',
  title: 'Paraphrase: shorten broll-c to 4 seconds from its start',
  difficulty: 'easy',
  fixtures: ['broll-c.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-c.mp4');
  },
  messages: ['Shorten broll-c to 4 seconds from its start.'],
  check(input) {
    const items = itemsOfType(input.doc, 'video');
    const item = items[0];
    if (!item) return finish([check('clip still on the timeline', false, 'no video items', 1)]);
    const src = keptSource(input.doc, input.assetIds['broll-c.mp4']);
    return finish([
      guard('clip still on the timeline', items.length === 1, `${items.length} video item(s)`, 1),
      check('plays 4 seconds', closeness(item.durationFrames / input.fps, 4, 0.12, 2), `lasts ${fmt(item.durationFrames / input.fps)}s`, 4),
      guard('keeps the start of the source', closeness(src[0]?.start ?? 99, 0, 0.1, 1), `source starts at ${fmt(src[0]?.start ?? -1)}s`, 2),
      guard('stays at the start of the timeline', item.startFrame <= 3, `starts at frame ${item.startFrame}`, 1),
    ]);
  },
};

export const squareFeed: Task = {
  id: 'ho-square',
  title: 'Paraphrase: make it square for the Instagram feed',
  difficulty: 'easy',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Make it square for the Instagram feed.'],
  check(input) {
    const { width, height } = input.doc.project;
    return finish([
      check('canvas is square', closeness(width / height, 1, 0.01, 0.2), `canvas is ${width}x${height}`, 4),
      guard('canvas resolution is usable', Math.min(width, height) >= 720, `short side ${Math.min(width, height)}px`, 1),
      guard('clip still on the timeline', itemsOfType(input.doc, 'video').length >= 1, `${itemsOfType(input.doc, 'video').length} video item(s)`, 1),
    ]);
  },
};

export const render720: Task = {
  id: 'ho-render-720',
  title: 'Paraphrase: render a 720p mp4',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Render a 720p mp4.'],
  timeoutMs: 12 * 60_000,
  check(input) {
    const done = input.exports.filter((e) => e.status === 'done');
    const out = done.find((e) => e.exists) ?? done[0];
    return finish([
      check('an export was started', input.exports.length > 0, `${input.exports.length} export(s)`, 1),
      check('export completed', done.length > 0, input.exports.map((e) => `${e.status}${e.error ? ` (${e.error})` : ''}`).join(', ') || 'none', 2),
      check('output file exists', Boolean(out?.exists && out.sizeBytes > 1000), out ? `${out.sizeBytes} bytes` : 'no file', 2),
      check('output is 720p', out?.height === 720, out ? `${out.width}x${out.height}` : 'no file', 3),
      check('output is an mp4', Boolean(out?.outputPath?.toLowerCase().endsWith('.mp4')), out?.outputPath ?? 'no file', 1),
    ]);
  },
};

export const moveBFirst: Task = {
  id: 'ho-b-first',
  title: 'New: move broll-b to the start',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: placeABC,
  messages: ['Move broll-b to the start.'],
  check: (input) => finish(orderChecks(input, 'BAC')),
};

export const aAfterC: Task = {
  id: 'ho-a-after-c',
  title: 'New: put broll-a after broll-c',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: placeABC,
  messages: ['Put broll-a after broll-c.'],
  check: (input) => finish(orderChecks(input, 'BCA')),
};

export const splitDeleteHalf: Task = {
  id: 'ho-split-delete',
  title: 'New: split the interview at 20 s and delete the second half',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Split the interview at 20 seconds and delete the second half.'],
  check(input) {
    const id = input.assetIds['interview.mp4'];
    const total = input.manifest.fixtures['interview.mp4'].durationSec;
    const head = keptFraction(input.doc, id, { start: 0, end: 20 });
    const tail = keptFraction(input.doc, id, { start: 21, end: total });
    const dur = docDurationSec(input.doc);
    return finish([
      guard('the first 20 s are kept', ramp(head, 0.8, 0.97), `${fmt(head * 100, 0)}% of 0-20 s is still there`, 3),
      check('the rest is gone', 1 - ramp(tail, 0.02, 0.5), `${fmt(tail * 100, 0)}% of 21 s-end is still there`, 4),
      check('timeline is about 20 s', closeness(dur, 20, 0.6, 5), `timeline is ${fmt(dur)}s`, 2),
    ]);
  },
};

export const thanksTitle: Task = {
  id: 'ho-thanks-title',
  title: 'New: a "Thanks for watching" title over the last 3 seconds',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
    await ctx.addToTimeline('broll-b.mp4');
  },
  messages: ['Add a "Thanks for watching" title for the last 3 seconds.'],
  check(input) {
    const total = docDurationSec(input.initialDoc);
    return finish([
      ...titleChecks(input, 'Thanks for watching', total - 3, 3),
      guard('the clips are untouched', itemsOfType(input.doc, 'video').length === 2 && closeness(docDurationSec(input.doc), total, 0.2, 2) >= 0.99, `${itemsOfType(input.doc, 'video').length} clips, ${fmt(docDurationSec(input.doc))}s`, 2),
    ]);
  },
};

export const musicLouder: Task = {
  id: 'ho-music-louder',
  title: 'New: make the music a bit louder',
  difficulty: 'easy',
  fixtures: ['broll-b.mp4', 'music.mp3'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-b.mp4');
    const id = await ctx.addToTimeline('music.mp3');
    projectService.apply([{ type: 'item.update', itemId: id, patch: { volume: 0.2 } }], 'user', 'setup: quiet music');
  },
  messages: ['Make the music a bit louder.'],
  check(input) {
    const music = itemsOfAsset(input.doc, input.assetIds['music.mp3']).find((i) => i.type === 'audio');
    const video = itemsOfType(input.doc, 'video')[0];
    const v = music?.volume ?? 0;
    return finish([
      check('music is louder', v > 0.2 * 1.15, `volume 0.2 -> ${fmt(v)}`, 3),
      guard('only a bit louder', v <= 0.7, `volume ${fmt(v)} (a bit: at most 0.7)`, 2),
      guard('music still on the timeline', music !== undefined, music ? 'present' : 'missing', 2),
      guard('video untouched', video !== undefined && video.volume === 1 && closeness(video.durationFrames / input.fps, input.manifest.fixtures['broll-b.mp4'].durationSec, 0.1, 1) >= 0.99, video ? `volume ${video.volume}` : 'missing', 1),
    ]);
  },
};

export const bakeryHighlight: Task = {
  id: 'ho-highlight',
  title: 'New: a 15 s highlight about the bakery',
  difficulty: 'hard',
  fixtures: ['talk.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('talk.mp4');
  },
  messages: ['Cut a 15-second highlight from the talk about the bakery.'],
  timeoutMs: 12 * 60_000,
  check(input) {
    const topic = input.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'customer')!;
    const fps = input.fps;
    const items = itemsOfAsset(input.doc, input.assetIds['talk.mp4']).filter((i) => i.type === 'video' || i.type === 'audio');
    let total = 0;
    let inTopic = 0;
    for (const i of items) {
      const start = (i.sourceInFrame ?? 0) / fps;
      const len = (i.durationFrames * i.speed) / fps;
      total += len;
      inTopic += coveredSec({ start, end: start + len }, [{ start: topic.startSec, end: topic.endSec }]);
    }
    const onTopic = total > 0 ? inTopic / total : 0;
    const dur = docDurationSec(input.doc);
    const media = mergeIntervals(items.map((i) => ({ start: i.startFrame / fps, end: (i.startFrame + i.durationFrames) / fps })));
    const layout = trackLayout(items.filter((i) => i.type === 'video'), fps);
    return finish([
      guard('has footage from the talk', items.length > 0, `${items.length} item(s)`, 1),
      check('length is about 15 s', closeness(dur, 15, 3, 12), `timeline is ${fmt(dur)}s`, 3),
      check('footage comes from the bakery story', ramp(onTopic, 0.4, 0.9), `${fmt(onTopic * 100, 0)}% of the used source is inside ${fmt(topic.startSec)}-${fmt(topic.endSec)}s`, 4),
      check('not the whole talk', media.reduce((a, m) => a + (m.end - m.start), 0) < 40, `${fmt(media.reduce((a, m) => a + (m.end - m.start), 0))}s of media on the timeline`, 1),
      guard('no big gaps', layout.maxGapSec <= 0.5, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

/** Run as a group with `--tasks heldout`. */
export const HELDOUT: Task[] = [
  tightenDeadAir,
  umsAndAhs,
  flub,
  shortenC,
  squareFeed,
  render720,
  moveBFirst,
  aAfterC,
  splitDeleteHalf,
  thanksTitle,
  musicLouder,
  bakeryHighlight,
];
