import type { Task } from '../types.ts';
import { check, finish, fmt, closeness, guard } from '../lib/score.ts';
import { itemsOfType } from '../lib/timeline.ts';
import { captionChecks, captionFontSize, captionItems, titleChecks } from '../lib/checks.ts';

/** Captions, titles, canvas. */

const interviewText = (input: Parameters<Task['check']>[0]): string =>
  input.manifest.fixtures['interview.mp4'].segments.map((s) => s.text ?? '').join(' ');

export const addCaptions: Task = {
  id: 'add-captions',
  title: 'Add captions to the interview',
  difficulty: 'easy',
  fixtures: ['interview.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('interview.mp4');
  },
  messages: ['Add captions to the interview.'],
  check: (input) => finish(captionChecks(input, interviewText(input))),
};

export const addTitle: Task = {
  id: 'add-title',
  title: 'Add a 3 s "Launch Day" title at the start',
  difficulty: 'easy',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Add a title that says "Launch Day" at the very start of the video, for 3 seconds.'],
  check: (input) => finish(titleChecks(input, 'Launch Day', 0, 3)),
};

export const vertical: Task = {
  id: 'vertical-reels',
  title: 'Switch to vertical 9:16 for Reels',
  difficulty: 'medium',
  fixtures: ['broll-a.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('broll-a.mp4');
  },
  messages: ['Change this project to vertical 9:16 so I can post it as an Instagram Reel.'],
  check(input) {
    const { width, height } = input.doc.project;
    const ratio = width / height;
    return finish([
      check('canvas is 9:16', closeness(ratio, 9 / 16, 0.01, 0.2), `canvas is ${width}x${height}`, 4),
      guard('canvas resolution is usable', Math.min(width, height) >= 720, `short side ${Math.min(width, height)}px`, 1),
      guard('clip still on the timeline', itemsOfType(input.doc, 'video').length >= 1, `${itemsOfType(input.doc, 'video').length} video item(s)`, 1),
    ]);
  },
};

export const captionsBiggerUndo: Task = {
  id: 'captions-bigger-undo',
  title: 'Multi-turn: captions, make them bigger, undo that',
  difficulty: 'hard',
  fixtures: ['interview.mp4'],
  setup: async (ctx) => {
    await ctx.addToTimeline('interview.mp4');
  },
  messages: ['Add captions to the interview.', 'Make them bigger.', 'Undo that.'],
  check(input) {
    const [d1, d2, d3] = input.docs;
    const checks = [] as ReturnType<typeof check>[];
    const n1 = d1 ? captionItems(d1).length : 0;
    const s1 = d1 ? captionFontSize(d1) : undefined;
    checks.push(check('turn 1: captions added', n1 > 0, `${n1} caption cards`, 2));
    const s2 = d2 ? captionFontSize(d2) : undefined;
    const allBigger = d2 && s1 !== undefined ? captionItems(d2).every((c) => c.props.style.fontSize >= s1 * 1.15) : false;
    checks.push(check('turn 2: captions are bigger', allBigger, `font size ${s1 ?? '?'} -> ${s2 ?? '?'}`, 3));
    checks.push(guard('turn 2: no cards lost', d2 ? captionItems(d2).length >= n1 && n1 > 0 : false, `${d2 ? captionItems(d2).length : 0} cards after`, 1));
    const s3 = d3 ? captionFontSize(d3) : undefined;
    const back = s1 !== undefined && s3 !== undefined && Math.abs(s3 - s1) <= 1 && d3 !== undefined && captionItems(d3).length === n1;
    checks.push(check('turn 3: undo restores the original size', back, `font size back to ${s3 ?? '?'} (original ${s1 ?? '?'}), ${d3 ? captionItems(d3).length : 0} cards`, 3));
    void fmt;
    return finish(checks);
  },
};
