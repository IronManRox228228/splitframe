import { applyOps, createItem } from '@cutboard/editor-core';
import type { Item, TranscriptWord } from '@cutboard/schema';
import { describe, expect, it } from 'vitest';
import {
  assembleOps,
  captionStyleOps,
  cutSourceRangesOps,
  fillerRanges,
  fitPicks,
  keptSourceSec,
  layoutInOrderOps,
  reorderedItems,
  sentencesOf,
  setClipRangeOps,
  snapToSpeech,
  sourceSpanToTimeline,
} from './compiler.ts';
import { fixtureSnapshot, speechTranscript } from './test-fixtures.ts';

const w = (text: string, startMs: number, endMs: number): TranscriptWord => ({ w: text, startMs, endMs });

describe('source to timeline', () => {
  it('maps a source range through the item window and speed', () => {
    const item = createItem('video', { id: 'i', trackId: 't', startFrame: 100, durationFrames: 150, assetId: 'a', sourceInFrame: 60, speed: 2 } as never);
    // plays source 2.0s..12.0s (150 frames * 2 / 30 = 10 s)
    expect(sourceSpanToTimeline(item, 30, 4, 6)).toEqual({ startFrame: 100 + 30, endFrame: 100 + 60 });
    expect(sourceSpanToTimeline(item, 30, 20, 30)).toBeNull();
    expect(sourceSpanToTimeline(item, 30, 0, 3)).toEqual({ startFrame: 100, endFrame: 100 + 15 });
  });
});

describe('cutSourceRangesOps', () => {
  it('removes the ranges and ripples, keeping the rest', () => {
    const snap = fixtureSnapshot();
    const { ops, cutSec } = cutSourceRangesOps(snap, 'ast_a', [[1, 2], [4, 5]]);
    expect(cutSec).toBeCloseTo(2, 5);
    const doc = applyOps(snap.doc, ops).doc;
    const a = doc.items.filter((i) => i.assetId === 'ast_a');
    expect(keptSourceSec(doc.items, 'ast_a', 30, 0, 6)).toBeCloseTo(4, 5);
    expect(keptSourceSec(doc.items, 'ast_a', 30, 1, 2)).toBeCloseTo(0, 5);
    expect(a.length).toBe(3);
  });
});

describe('fillers', () => {
  it('finds sound fillers, stretched spellings and phrases, and caps ASR-stretched ends', () => {
    const words = [w('We', 0, 300), w('started,', 300, 700), w('UMM,', 1000, 5000), w('and', 5200, 5400), w('so', 5400, 5600), w('you', 6000, 6200), w('know,', 6200, 6800), w('it', 6900, 7000), w('uh', 8000, 8300)];
    const r = fillerRanges(words);
    expect(r).toEqual([
      [0.65, 1.8], // reaches back 0.35 s (not before the previous word's start + 0.2 s); capped at 0.8 s after the ASR start
      [5.65, 6.8],
      [7.65, 8.3],
    ]);
  });
  it('honours a custom vocabulary and does not touch other words', () => {
    const words = [w('album', 0, 400), w('like', 400, 800), w('um', 900, 1200)];
    expect(fillerRanges(words, ['like'])).toEqual([[0.2, 0.8]]);
    expect(fillerRanges([w('umbrella', 0, 500), w('humming', 500, 900)])).toEqual([]);
  });
});

describe('sentences and picks', () => {
  const t = speechTranscript('x', ['One two three four.', 'Five six seven eight.', 'Nine ten eleven twelve.', 'Thirteen fourteen fifteen sixteen.'], 0, 1);
  const sentences = sentencesOf(t.words);

  it('splits at punctuation with times', () => {
    expect(sentences).toHaveLength(4);
    expect(sentences[1]).toMatchObject({ index: 2, startSec: 2.6, endSec: 4.2, text: 'Five six seven eight.' });
  });

  it('trims silence stretched onto sentence ends', () => {
    const snapped = snapToSpeech(sentences, [{ startMs: 1300, endMs: 2600 }], 0.1);
    expect(snapped[0]!.endSec).toBeCloseTo(1.4, 5);
    expect(snapped[1]!.endSec).toBe(sentences[1]!.endSec);
  });

  it('fits whole sentences to the target length and joins close neighbours', () => {
    const all = fitPicks(sentences, [{ first: 1, last: 4 }], undefined);
    expect(all.sentenceCount).toBe(4);
    expect(all.ranges).toHaveLength(4); // 1 s gaps stay cut out
    const fit = fitPicks(sentences, [{ first: 1, last: 4 }], 3.4);
    expect(fit.sentenceCount).toBe(2);
    expect(fit.totalSec).toBeCloseTo(3.2, 5);
    const joined = fitPicks(sentences, [{ first: 2, last: 3 }], undefined, 1.5);
    expect(joined.ranges).toHaveLength(1);
  });

  it('clamps bad indices and dedupes overlapping picks', () => {
    const r = fitPicks(sentences, [{ first: 3, last: 99 }, { first: 4, last: 4 }, { first: 0, last: 1 }], undefined);
    expect(r.sentenceCount).toBe(3);
  });
});

describe('assembleOps', () => {
  it('replaces the asset on its track with the ranges back to back', () => {
    const snap = fixtureSnapshot();
    const { ops, totalFrames, itemIds } = assembleOps(snap, [
      { assetId: 'ast_int', startSec: 10, endSec: 14 },
      { assetId: 'ast_int', startSec: 30, endSec: 35 },
    ]);
    expect(totalFrames).toBe(9 * 30);
    const doc = applyOps(snap.doc, ops).doc;
    const added = doc.items.filter((i) => itemIds.includes(i.id));
    expect(added.map((i) => [i.startFrame, i.durationFrames, i.sourceInFrame])).toEqual([
      [0, 120, 300],
      [120, 150, 900],
    ]);
  });
  it('drops items of the same asset already on the track and starts where they started', () => {
    const snap = fixtureSnapshot();
    const { ops } = assembleOps(snap, [{ assetId: 'ast_b', startSec: 0, endSec: 2 }]);
    const doc = applyOps(snap.doc, ops).doc;
    const b = doc.items.filter((i) => i.assetId === 'ast_b');
    expect(b).toHaveLength(1);
    expect(b[0]!.startFrame).toBe(180);
    expect(b[0]!.durationFrames).toBe(60);
  });
});

describe('trim and reorder', () => {
  it('keeps source seconds [from, to] and ripples followers', () => {
    const snap = fixtureSnapshot();
    const a = snap.doc.items.find((i) => i.id === 'itm_a')!;
    const { ops, newDurationFrames } = setClipRangeOps(snap, a, 0, 4, true);
    expect(newDurationFrames).toBe(120);
    const doc = applyOps(snap.doc, ops).doc;
    expect(doc.items.find((i) => i.id === 'itm_a')!.durationFrames).toBe(120);
    expect(doc.items.find((i) => i.id === 'itm_b')!.startFrame).toBe(120);
    expect(doc.items.find((i) => i.id === 'itm_c')!.startFrame).toBe(300);
  });
  it('rejects empty ranges and clamps to the source length', () => {
    const snap = fixtureSnapshot();
    const a = snap.doc.items.find((i) => i.id === 'itm_a')!;
    expect(() => setClipRangeOps(snap, a, 5, 5, false)).toThrow(/empty/);
    expect(setClipRangeOps(snap, a, 0, 99, false).newDurationFrames).toBe(180);
  });
  it('reorders a track contiguously', () => {
    const snap = fixtureSnapshot();
    const [a, b, c] = ['itm_a', 'itm_b', 'itm_c'].map((id) => snap.doc.items.find((i) => i.id === id)!) as [Item, Item, Item];
    const order = reorderedItems([a, b, c], c, a, 'before');
    expect(order.map((i) => i.id)).toEqual(['itm_c', 'itm_a', 'itm_b']);
    const doc = applyOps(snap.doc, layoutInOrderOps(order)).doc;
    const starts = Object.fromEntries(doc.items.map((i) => [i.id, i.startFrame]));
    expect([starts.itm_c, starts.itm_a, starts.itm_b]).toEqual([0, 180, 360]);
  });
});

describe('captionStyleOps', () => {
  it('scales font size on every card in one batch and leaves other items alone', () => {
    const snap = fixtureSnapshot();
    const text = snap.doc.tracks.find((t) => t.kind === 'text')!.id;
    const card = (id: string) => ({ id, trackId: text, type: 'caption', startFrame: 0, durationFrames: 30, props: { words: [], style: { fontSize: 60 }, mode: 'phrase', maxWordsPerCard: 4 } }) as never as Item;
    const items = [card('c1'), card('c2'), snap.doc.items[0]!];
    const ops = captionStyleOps(items, { sizeFactor: 1.5 });
    expect(ops).toHaveLength(1);
    expect(ops[0]!.type).toBe('batch');
    expect((ops[0] as unknown as { ops: unknown[] }).ops).toHaveLength(2);
    expect(JSON.stringify(ops)).toContain('"fontSize":90');
  });
});
