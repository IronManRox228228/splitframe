import type { CheckInput, Task } from '../types.ts';
import { check, finish, fmt, ramp, closeness, guard } from '../lib/score.ts';
import { coveredSec, docDurationSec, itemsOfAsset, keptFraction, mergeIntervals, trackLayout, type Interval } from '../lib/timeline.ts';
import { captionChecks, musicChecks, pausesRemoved, retakeRemoved, speechKept, titleChecks, wantedSpeech } from '../lib/checks.ts';

/** Music under speech, the customer-story teaser, export, and the all-in-one request. */

const interviewSpeech = (input: CheckInput): Interval[] =>
  input.manifest.fixtures['interview.mp4'].segments.filter((s) => s.kind === 'speech').map((s) => ({ start: s.startSec, end: s.endSec }));

export const musicDuck: Task = {
  id: 'music-under-speech',
  title: 'Add music under the interview and duck it under speech',
  difficulty: 'medium',
  fixtures: ['interview.mp4', 'music.mp3'],
  setup: async (ctx) => {
    await ctx.addToTimeline('interview.mp4');
  },
  messages: ['Add the music track under the whole interview, keep it quiet, and duck it whenever someone is talking.'],
  check(input) {
    const cover = input.manifest.fixtures['interview.mp4'].durationSec;
    return finish(musicChecks(input, { speech: interviewSpeech(input), quiet: 0.5, duck: true, coverSec: cover }));
  },
};

export const teaser: Task = {
  id: 'customer-teaser',
  title: 'Make a 30 s teaser about the customer story',
  difficulty: 'hard',
  fixtures: ['talk.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('talk.mp4');
  },
  messages: ['Make a 30-second teaser from the talk about the customer story only (the bakery).'],
  timeoutMs: 12 * 60_000,
  check(input) {
    const topic = input.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'customer')!;
    const id = input.assetIds['talk.mp4'];
    const fps = input.fps;
    const items = itemsOfAsset(input.doc, id).filter((i) => i.type === 'video' || i.type === 'audio');
    // timeline seconds of media, split by which topic its source belongs to
    let total = 0;
    let inTopic = 0;
    for (const i of items) {
      const start = (i.sourceInFrame ?? 0) / fps;
      const len = (i.durationFrames * i.speed) / fps;
      total += len;
      inTopic += coveredSec({ start, end: start + len }, [{ start: topic.startSec, end: topic.endSec }]);
    }
    // an item can be both a video and an audio entry of the same source; count seconds once per timeline position
    const videoSec = mergeIntervals(items.map((i) => ({ start: i.startFrame / fps, end: (i.startFrame + i.durationFrames) / fps }))).reduce((a, m) => a + (m.end - m.start), 0);
    const onTopic = total > 0 ? inTopic / total : 0;
    const dur = docDurationSec(input.doc);
    const layout = trackLayout(items.filter((i) => i.type === 'video'), fps);
    return finish([
      guard('has footage from the talk', items.length > 0, `${items.length} item(s)`, 1),
      check('length is 25-35 s', closeness(dur, 30, 5, 25), `timeline is ${fmt(dur)}s`, 3),
      check('footage comes from the customer story', ramp(onTopic, 0.4, 0.9), `${fmt(onTopic * 100, 0)}% of the used source is inside ${fmt(topic.startSec)}-${fmt(topic.endSec)}s`, 4),
      check('not the whole talk', videoSec > 0 && videoSec < 60, `${fmt(videoSec)}s of media on the timeline`, 1),
      guard('no big gaps', layout.maxGapSec <= 0.5, `largest gap ${fmt(layout.maxGapSec)}s`, 1),
    ]);
  },
};

export const exportSd: Task = {
  id: 'export-720p',
  title: 'Export at 720p',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Export the video at 720p.'],
  timeoutMs: 12 * 60_000,
  check(input) {
    const done = input.exports.filter((e) => e.status === 'done');
    const out = done.find((e) => e.exists) ?? done[0];
    return finish([
      check('an export was started', input.exports.length > 0, `${input.exports.length} export(s)`, 1),
      check('export completed', done.length > 0, input.exports.map((e) => `${e.status}${e.error ? ` (${e.error})` : ''}`).join(', ') || 'none', 2),
      check('output file exists', Boolean(out?.exists && out.sizeBytes > 1000), out ? `${out.sizeBytes} bytes` : 'no file', 2),
      check('output is 720p', out?.height === 720, out ? `${out.width}x${out.height}` : 'no file', 3),
    ]);
  },
};

export const compound: Task = {
  id: 'compound-edit',
  title: 'Rough cut + captions + music + title in one request',
  difficulty: 'hard',
  fixtures: ['interview.mp4', 'music.mp3'],
  setup: async (ctx) => {
    await ctx.addToTimeline('interview.mp4');
  },
  messages: [
    'Make this interview ready to post: cut out the long pauses and the repeated take, add captions, put the music track quietly underneath, and add a title that says "Launch Day" for the first 3 seconds.',
  ],
  timeoutMs: 15 * 60_000,
  check(input) {
    const text = input.manifest.fixtures['interview.mp4'].segments.map((s) => s.text ?? '').join(' ');
    const dur = docDurationSec(input.doc);
    const cap = captionChecks(input, text).map((c) => ({ ...c, name: `captions: ${c.name}`, weight: c.weight * 0.5 }));
    return finish([
      pausesRemoved(input, 0.5, 2),
      ...retakeRemoved(input, 2),
      speechKept(input, wantedSpeech(input, { dropRetake: true }), 'speech preserved', 2),
      ...cap,
      ...musicChecks(input, { quiet: 0.5, duck: false, coverSec: Math.max(1, dur) }).map((c) => ({ ...c, name: `music: ${c.name}` })),
      ...titleChecks(input, 'Launch Day', 0, 3, 0.7),
    ]);
  },
};

void keptFraction;
