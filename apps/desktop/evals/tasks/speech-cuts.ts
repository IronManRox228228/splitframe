import type { Task } from '../types.ts';
import { check, finish, fmt, closeness } from '../lib/score.ts';
import { docDurationSec } from '../lib/timeline.ts';
import { fillersRemoved, pausesRemoved, retakeRemoved, speechKept, wantedSpeech } from '../lib/checks.ts';

/** Transcript-driven cuts on the interview: pauses, filler words, the retake. */

const placeInterview: Task['setup'] = async (ctx) => {
  await ctx.addToTimeline('interview.mp4');
};

export const removePauses: Task = {
  id: 'remove-pauses',
  title: 'Remove pauses over 0.5 s from the interview',
  difficulty: 'easy',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Remove all the pauses longer than half a second from the interview.'],
  check(input) {
    const truth = input.manifest.fixtures['interview.mp4'];
    const before = docDurationSec(input.initialDoc);
    const shrink = before - docDurationSec(input.doc);
    return finish([
      pausesRemoved(input, 0.5, 4),
      speechKept(input, wantedSpeech(input, {}), 'speech preserved', 3),
      check('duration shrinks by about the pause total', closeness(shrink, truth.totalPauseSec, 2.5, 6), `shrank ${fmt(shrink)}s, pauses total ${truth.totalPauseSec}s`, 2),
    ]);
  },
};

export const removeFillers: Task = {
  id: 'remove-fillers',
  title: 'Remove filler words (um, uh, you know)',
  difficulty: 'hard',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['Cut out all the "um", "uh" and "you know" filler words from the interview.'],
  check(input) {
    return finish([fillersRemoved(input, 4), speechKept(input, wantedSpeech(input, { dropFillers: true }), 'real speech preserved', 3)]);
  },
};

export const removeRetake: Task = {
  id: 'remove-retake',
  title: 'Remove the abandoned retake',
  difficulty: 'medium',
  fixtures: ['interview.mp4'],
  setup: placeInterview,
  messages: ['In the interview I started one sentence ("So the reason we started this company was...") and gave up halfway, then said it again properly. Remove the bad attempt and keep the clean one.'],
  check(input) {
    return finish([...retakeRemoved(input, 3), speechKept(input, wantedSpeech(input, { dropRetake: true }), 'rest of the speech preserved', 2)]);
  },
};
