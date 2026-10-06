import { describe, expect, it } from 'vitest';
import { captionCardAt, captionClockMs, type CaptionWord } from './captions.ts';

const words: CaptionWord[] = [
  { w: 'one', startMs: 0, endMs: 300 },
  { w: 'two', startMs: 400, endMs: 700 },
  { w: 'three', startMs: 800, endMs: 1100 },
  { w: 'four', startMs: 1300, endMs: 1600 },
];

describe('captionClockMs', () => {
  it('is relative to the caption item, whatever its timeline position', () => {
    expect(captionClockMs(300, 300, 30)).toBe(0);
    expect(captionClockMs(315, 300, 30)).toBe(500);
  });
});

describe('captionCardAt', () => {
  it('shows nothing before the first word', () => {
    expect(captionCardAt(words, -10, 4)).toBeNull();
  });

  it('highlights the word being spoken', () => {
    const r = captionCardAt(words, 450, 4)!;
    expect(r.activeIdx).toBe(1);
    expect(r.card.map((w) => w.w)).toEqual(['one', 'two', 'three', 'four']);
  });

  it('keeps the current card up between words instead of jumping back to the first card', () => {
    const r = captionCardAt(words, 1200, 2)!; // gap between "three" and "four"
    expect(r.activeIdx).toBe(-1);
    expect(r.card.map((w) => w.w)).toEqual(['three', 'four']); // the second card starts at "three"
  });

  it('pages into later cards', () => {
    const r = captionCardAt(words, 1400, 2)!;
    expect(r.card.map((w) => w.w)).toEqual(['three', 'four']);
    expect(r.activeIdx).toBe(3);
  });
});
