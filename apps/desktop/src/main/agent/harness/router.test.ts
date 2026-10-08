import { describe, expect, it } from 'vitest';
import { looksCompound, needsPlan, routeByRules, routeNoModel, toolsetFor } from './router.ts';

describe('router', () => {
  it.each([
    ['Add a title that says "Launch Day" at the very start of the video, for 3 seconds.', ['title']],
    ['Trim broll-c so that only its first 4 seconds play.', ['clips']],
    ['Delete the selected clip.', ['clips']],
    ['Move broll-c so it plays first, before broll-a. Keep everything else in the same order.', ['clips']],
    ['Change this project to vertical 9:16 so I can post it as an Instagram Reel.', ['canvas']],
    ['Add captions to the interview.', ['captions']],
    ['Make them bigger.', []],
    ['Undo that.', ['undo']],
    ['Remove all the pauses longer than half a second from the interview.', ['pauses']],
    ['Cut out all the "um", "uh" and "you know" filler words from the interview.', ['fillers']],
    ['Export the video at 720p.', ['export']],
    ['Cut my three b-roll clips to the beat of the music track.', ['music', 'beats']],
  ])('%s', (msg, intents) => {
    const got = routeByRules(msg);
    for (const i of intents) expect(got).toContain(i);
    if (intents.length === 1) expect(got).toEqual(intents);
  });

  it('plans compound and judgment requests only', () => {
    const compound = 'Make this interview ready to post: cut out the long pauses and the repeated take, add captions, put the music track quietly underneath, and add a title that says "Launch Day".';
    expect(needsPlan(routeByRules(compound))).toBe(true);
    expect(needsPlan(routeByRules('Make a 30-second teaser from the talk about the customer story only.'))).toBe(true);
    expect(needsPlan(routeByRules('Remove the pauses and the filler words.'))).toBe(false);
    expect(needsPlan(routeByRules('Add captions to the interview.'))).toBe(false);
  });

  it('falls through to the model when no rule fires, and spots questions', () => {
    expect(routeNoModel('make it feel more energetic')).toBeNull();
    expect(routeNoModel('How long is the interview?')?.intents).toEqual(['question']);
  });

  it('reads style follow-ups against the recent turns', () => {
    expect(routeNoModel('Make them bigger.', 'Added 40 caption cards')?.intents).toEqual(['captions']);
    expect(routeNoModel('Make them bigger.', 'Deleted a clip')).toBeNull();
  });

  it('knows filler vocabulary in plural and stretched forms, and general retake wording', () => {
    expect(routeByRules('lose the ums and uhs')).toContain('fillers');
    expect(routeByRules('that first attempt was a flub, cut it')).toContain('retakes');
  });

  it('does not trust a lone weak word, and detects compound requests', () => {
    expect(routeNoModel('play it after lunch')).toBeNull();
    expect(looksCompound('trim it, then slow it down')).toBe(true);
    expect(looksCompound('trim the clip')).toBe(false);
  });

  it('exposes 3-8 tools, never the whole catalog', () => {
    expect(toolsetFor(['clips'])).toHaveLength(7);
    expect(toolsetFor(['captions'])).toEqual(['addCaptions', 'styleCaptions']);
    expect(toolsetFor(['clips', 'assemble', 'music', 'beats', 'captions']).length).toBeLessThanOrEqual(8);
  });
});
