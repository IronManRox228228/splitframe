import type { CheckInput, Task } from '../types.ts';
import { check, closeness, finish, fmt, guard, ramp } from '../lib/score.ts';
import { coveredSec, docDurationSec, itemsOfAsset, itemsOfType, keptFraction, keptSource, mergeIntervals, trackLayout } from '../lib/timeline.ts';
import { captionChecks, captionItems, pausesRemoved, speechKept, titleChecks, wantedSpeech } from '../lib/checks.ts';
import { projectService } from '../../src/main/project-service.ts';

/**
 * Second held-out set (`--tasks heldout2`): written as a user would say it, in wording that differs from
 * the original 15 and from heldout.ts, with new kinds of request: two actions in one message, timing
 * relative to the end, references by what a clip shows, colours, speed. Same fixtures and check helpers.
 * Not for tuning: when one fails, fix a general mechanism, never the wording.
 */

const placeInterview: Task['setup'] = async (ctx) => {
  await ctx.addToTimeline('interview.mp4');
};

const placeABC: Task['setup'] = async (ctx) => {
  for (const f of ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'] as const) await ctx.addToTimeline(f);
};

const brollOrder = (input: CheckInput): string => {
  const names: Record<string, string> = {};
  for (const [name, id] of Object.entries(input.assetIds)) names[id] = name.replace('broll-', '').replace('.mp4', '').toUpperCase();
  return itemsOfType(input.doc, 'video').map((i) => names[i.assetId ?? ''] ?? '?').join('');
};

const interviewTranscript = (input: CheckInput): string => input.transcripts.find((t) => t.assetId === input.assetIds['interview.mp4'])?.words.map((w) => w.w).join(' ') ?? '';

/** Parse #rgb / #rrggbb / rgb() / a few colour names into 0-255 channels. */
function rgbOf(color: string): [number, number, number] | null {
  const c = color.trim().toLowerCase();
  const names: Record<string, string> = { yellow: '#ffff00', gold: '#ffd700' };
  const hex = names[c] ?? c;
  const m3 = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/.exec(hex);
  if (m3) return [parseInt(m3[1]! + m3[1], 16), parseInt(m3[2]! + m3[2], 16), parseInt(m3[3]! + m3[3], 16)];
  const m6 = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})/.exec(hex);
  if (m6) return [parseInt(m6[1]!, 16), parseInt(m6[2]!, 16), parseInt(m6[3]!, 16)];
  const rgb = /^rgba?\(\s*(\d+)[ ,]+(\d+)[ ,]+(\d+)/.exec(hex);
  return rgb ? [Number(rgb[1]), Number(rgb[2]), Number(rgb[3])] : null;
}
const isYellow = ([r, g, b]: [number, number, number]): boolean => r >= 200 && g >= 170 && b <= 120;

export const awkwardGaps: Task = {
  id: 'h2-gaps',
  title: 'Get rid of the awkward stretches where nobody speaks',
  difficulty: 'easy',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Get rid of the awkward stretches where nobody is saying anything.'],
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

export const allFillers: Task = {
  id: 'h2-fillers',
  title: 'Cut every filler: the ums, uhs and you-knows',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Cut every filler out of the interview: the ums, the uhs and the you-knows.'],
  check(input) {
    const id = input.assetIds['interview.mp4'];
    const fillers = input.manifest.fixtures['interview.mp4'].fillers;
    const gone = fillers.filter((f) => keptFraction(input.doc, id, { start: f.startSec, end: f.endSec }) <= 0.25);
    return finish([
      check('every filler removed', fillers.length ? gone.length / fillers.length : 1, `${gone.length}/${fillers.length} cut (${fillers.map((f) => f.word).join(', ')})`, 4),
      speechKept(input, wantedSpeech(input, { dropFillers: true }), 'real speech preserved', 3),
    ]);
  },
};

export const lastFive: Task = {
  id: 'h2-last-5s',
  title: 'Chop the last 5 seconds off the interview',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Chop the last 5 seconds off the end of the interview.'],
  check(input) {
    const id = input.assetIds['interview.mp4'];
    const total = input.manifest.fixtures['interview.mp4'].durationSec;
    const tail = keptFraction(input.doc, id, { start: total - 4.5, end: total });
    const head = keptFraction(input.doc, id, { start: 0, end: total - 5.5 });
    const dur = docDurationSec(input.doc);
    return finish([
      check('the last 5 seconds are gone', 1 - ramp(tail, 0.05, 0.6), `${fmt(tail * 100, 0)}% of the final 4.5 s is still there`, 4),
      check('the video is about 5 s shorter', closeness(dur, total - 5, 0.6, 4), `timeline is ${fmt(dur)}s (want ${fmt(total - 5)}s)`, 2),
      guard('the rest is kept', ramp(head, 0.8, 0.97), `${fmt(head * 100, 0)}% of the rest is still there`, 3),
    ]);
  },
};

export const dropBRollB: Task = {
  id: 'h2-drop-label-b',
  title: 'Remove the clip that says BROLL B',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: placeABC,
  messages: ['Remove the clip that says BROLL B.'],
  check(input) {
    const items = itemsOfType(input.doc, 'video');
    const got = brollOrder(input);
    const layout = trackLayout(items, input.fps);
    const f = input.manifest.fixtures;
    const want = f['broll-a.mp4'].durationSec + f['broll-c.mp4'].durationSec;
    return finish([
      check('the BROLL B clip is gone', !got.includes('B'), `clips now: ${got.split('').join(', ') || 'none'}`, 4),
      guard('the other two are kept', got === 'AC', `clips now: ${got.split('').join(', ') || 'none'}`, 3),
      check('the gap closed up', layout.maxGapSec <= 0.1 && closeness(docDurationSec(input.doc), want, 0.3, 3) >= 0.99, `largest gap ${fmt(layout.maxGapSec)}s, ${fmt(docDurationSec(input.doc))}s long (want ${want}s)`, 2),
    ]);
  },
};

export const openWithC: Task = {
  id: 'h2-open-c',
  title: 'Open with the BROLL C clip, rest as they are',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: placeABC,
  messages: ['Open with the BROLL C clip and keep the other two in the order they are in now.'],
  check(input) {
    const items = itemsOfType(input.doc, 'video');
    const layout = trackLayout(items, input.fps);
    const f = input.manifest.fixtures;
    const total = f['broll-a.mp4'].durationSec + f['broll-b.mp4'].durationSec + f['broll-c.mp4'].durationSec;
    const got = brollOrder(input);
    return finish([
      check('order is C, A, B', got === 'CAB', `order ${got.split('').join(' > ')}`, 4),
      guard('all three clips intact', items.length === 3 && closeness(docDurationSec(input.doc), total, 0.3, 3) >= 0.99, `${items.length} items, ${fmt(docDurationSec(input.doc))}s (want ${total}s)`, 2),
      guard('no overlaps', layout.overlapSec <= 0.05, `overlap ${fmt(layout.overlapSec)}s`, 1),
      guard('no gaps', layout.maxGapSec <= 0.1, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

export const yellowCaptions: Task = {
  id: 'h2-yellow-captions',
  title: 'Add captions and make them yellow',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Put captions on the interview and make them yellow.'],
  check(input) {
    const caps = captionItems(input.doc);
    const colors = caps.map((c) => rgbOf(String(c.props.style.color ?? '')));
    const yellow = colors.filter((c) => c && isYellow(c)).length;
    return finish([
      ...captionChecks(input, interviewTranscript(input)),
      check('the captions are yellow', caps.length ? yellow / caps.length : 0, `${yellow}/${caps.length} cards yellow (first: ${caps[0]?.props.style.color ?? 'none'})`, 4),
    ]);
  },
};

export const slowBAndTitle: Task = {
  id: 'h2-slow-title',
  title: 'Slow broll-b to half speed and add a "Summer Sale" title at the start',
  difficulty: 'hard',
  fixtures: ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'],
  setup: placeABC,
  messages: ['Slow broll-b down to half speed, and put a title that says "Summer Sale" right at the start.'],
  check(input) {
    const b = itemsOfAsset(input.doc, input.assetIds['broll-b.mp4']).find((i) => i.type === 'video');
    const items = itemsOfType(input.doc, 'video');
    const layout = trackLayout(items, input.fps);
    const f = input.manifest.fixtures;
    return finish([
      check('broll-b plays at half speed', b ? closeness(b.speed, 0.5, 0.05, 0.4) : 0, b ? `speed ${fmt(b.speed)}x` : 'broll-b is missing', 4),
      check('broll-b now lasts about twice as long', b ? closeness(b.durationFrames / input.fps, f['broll-b.mp4'].durationSec * 2, 0.3, 3) : 0, b ? `lasts ${fmt(b.durationFrames / input.fps)}s` : 'missing', 2),
      ...titleChecks(input, 'Summer Sale', 0, 3),
      guard('all three clips still there', items.length === 3, `${items.length} video clips`, 2),
      guard('no overlaps', layout.overlapSec <= 0.05, `overlap ${fmt(layout.overlapSec)}s`, 2),
      guard('no gaps', layout.maxGapSec <= 0.1, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

export const musicHalf: Task = {
  id: 'h2-music-half',
  title: 'Bring the music down to about half',
  difficulty: 'easy',
  fixtures: ['broll-b.mp4', 'music.mp3'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-b.mp4');
    const id = await ctx.addToTimeline('music.mp3');
    projectService.apply([{ type: 'item.update', itemId: id, patch: { volume: 0.8 } }], 'user', 'setup: loud music');
  },
  messages: ['The music is too loud, bring it down to about half.'],
  check(input) {
    const music = itemsOfAsset(input.doc, input.assetIds['music.mp3']).find((i) => i.type === 'audio');
    const video = itemsOfType(input.doc, 'video')[0];
    const v = music?.volume ?? 0;
    return finish([
      check('music is about half as loud', closeness(v, 0.4, 0.08, 0.3), `volume 0.8 -> ${fmt(v)} (want about 0.4)`, 4),
      guard('music still on the timeline', music !== undefined, music ? 'present' : 'missing', 2),
      guard('video untouched', video !== undefined && video.volume === 1 && closeness(video.durationFrames / input.fps, input.manifest.fixtures['broll-b.mp4'].durationSec, 0.1, 1) >= 0.99, video ? `volume ${video.volume}` : 'missing', 1),
    ]);
  },
};

export const loseFirstTwo: Task = {
  id: 'h2-lose-2s',
  title: 'Lose the first 2 seconds of broll-a',
  difficulty: 'easy',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Lose the first 2 seconds of broll-a.'],
  check(input) {
    const items = itemsOfType(input.doc, 'video');
    const item = items[0];
    if (!item) return finish([check('clip still on the timeline', false, 'no video items', 1)]);
    const src = keptSource(input.doc, input.assetIds['broll-a.mp4']);
    const total = input.manifest.fixtures['broll-a.mp4'].durationSec;
    return finish([
      check('now 3 seconds long', closeness(item.durationFrames / input.fps, total - 2, 0.15, 1.5), `lasts ${fmt(item.durationFrames / input.fps)}s (want ${total - 2}s)`, 3),
      check('starts 2 seconds into the source', closeness(src[0]?.start ?? -9, 2, 0.12, 1.5), `source starts at ${fmt(src[0]?.start ?? -1)}s`, 4),
      guard('one clip, at the start of the timeline', items.length === 1 && item.startFrame <= 3, `${items.length} clip(s), starts at frame ${item.startFrame}`, 2),
    ]);
  },
};

export const verticalWithCaptions: Task = {
  id: 'h2-vertical-captions',
  title: 'Vertical for TikTok, with captions',
  difficulty: 'hard',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Make this a vertical video for TikTok and put captions on it.'],
  check(input) {
    const { width, height } = input.doc.project;
    return finish([
      check('canvas is 9:16', closeness(width / height, 9 / 16, 0.01, 0.2), `canvas is ${width}x${height}`, 4),
      ...captionChecks(input, interviewTranscript(input)),
      guard('the speech is still there', ramp(keptFraction(input.doc, input.assetIds['interview.mp4'], { start: 0, end: 59 }), 0.8, 0.97), 'speech kept', 2),
    ]);
  },
};

export const demoClip: Task = {
  id: 'h2-demo-20s',
  title: 'About 20 seconds covering the product walkthrough',
  difficulty: 'hard',
  fixtures: ['talk.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('talk.mp4');
  },
  messages: ['Pull about 20 seconds out of the talk that cover the product walkthrough.'],
  timeoutMs: 12 * 60_000,
  check(input) {
    const topic = input.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'demo')!;
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
    const mediaSec = media.reduce((a, m) => a + (m.end - m.start), 0);
    const layout = trackLayout(items.filter((i) => i.type === 'video'), fps);
    return finish([
      guard('has footage from the talk', items.length > 0, `${items.length} item(s)`, 1),
      check('length is about 20 s', closeness(dur, 20, 3, 14), `timeline is ${fmt(dur)}s`, 3),
      check('footage comes from the product demo', ramp(onTopic, 0.4, 0.9), `${fmt(onTopic * 100, 0)}% of the used source is inside ${fmt(topic.startSec)}-${fmt(topic.endSec)}s`, 4),
      check('not the whole talk', mediaSec < 45, `${fmt(mediaSec)}s of media on the timeline`, 1),
      guard('no big gaps', layout.maxGapSec <= 0.5, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

export const cutRange: Task = {
  id: 'h2-cut-range',
  title: 'Cut out everything between 10 and 20 seconds',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Cut out everything between the 10 second and the 20 second mark of the interview.'],
  check(input) {
    const id = input.assetIds['interview.mp4'];
    const total = input.manifest.fixtures['interview.mp4'].durationSec;
    const middle = keptFraction(input.doc, id, { start: 10.5, end: 19.5 });
    const before = keptFraction(input.doc, id, { start: 0, end: 9.5 });
    const after = keptFraction(input.doc, id, { start: 20.5, end: total });
    const dur = docDurationSec(input.doc);
    return finish([
      check('10-20 s is gone', 1 - ramp(middle, 0.05, 0.6), `${fmt(middle * 100, 0)}% of 10.5-19.5 s is still there`, 4),
      check('the video is about 10 s shorter', closeness(dur, total - 10, 0.8, 5), `timeline is ${fmt(dur)}s (want ${fmt(total - 10)}s)`, 2),
      guard('everything else is kept', ramp(Math.min(before, after), 0.8, 0.97), `${fmt(before * 100, 0)}% before, ${fmt(after * 100, 0)}% after`, 3),
      guard('the gap closed up', trackLayout(itemsOfType(input.doc, 'video'), input.fps).maxGapSec <= 0.2, 'no gap left where the cut was', 1),
    ]);
  },
};

/** Run as a group with `--tasks heldout2`. */
export const HELDOUT2: Task[] = [
  awkwardGaps,
  allFillers,
  lastFive,
  dropBRollB,
  openWithC,
  yellowCaptions,
  slowBAndTitle,
  musicHalf,
  loseFirstTwo,
  verticalWithCaptions,
  demoClip,
  cutRange,
];
