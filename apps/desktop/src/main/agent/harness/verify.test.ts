import { createItem } from '@cutboard/editor-core';
import { describe, expect, it } from 'vitest';
import { fixtureDoc } from './test-fixtures.ts';
import * as V from './verify.ts';

const msgs = (f: { message: string }[]) => f.map((x) => x.message).join(' | ');

describe('verifier', () => {
  it('flags a step that changed nothing', () => {
    const d = fixtureDoc();
    expect(V.changedSomething(d, structuredClone(d), 'the cut')[0]!.message).toContain('changed nothing');
    const e = structuredClone(d);
    e.items[0]!.durationFrames -= 10;
    expect(V.changedSomething(d, e, 'the cut')).toEqual([]);
  });

  it('detects new overlaps but not old ones', () => {
    const d = fixtureDoc();
    const e = structuredClone(d);
    e.items.find((i) => i.id === 'itm_b')!.startFrame = 90;
    expect(msgs(V.newOverlaps(d, e))).toContain('overlap by 3s');
    expect(V.newOverlaps(e, e)).toEqual([]);
  });

  it('checks item range, order, gaps and deletion', () => {
    const d = fixtureDoc();
    expect(V.itemRange(d, 'itm_a', 0, 180)).toEqual([]);
    expect(msgs(V.itemRange(d, 'itm_a', 0, 120))).toContain('lasts 6s, expected 4s');
    expect(V.inOrder(d, ['itm_a', 'itm_b', 'itm_c'])).toEqual([]);
    expect(msgs(V.inOrder(d, ['itm_b', 'itm_a']))).toContain('overlap');
    const gap = structuredClone(d);
    gap.items.find((i) => i.id === 'itm_c')!.startFrame = 450;
    expect(msgs(V.inOrder(gap, ['itm_b', 'itm_c']))).toContain('gap');
    expect(V.itemsGone(d, ['itm_a'])).not.toEqual([]);
    expect(V.itemsGone(d, ['nope'])).toEqual([]);
  });

  it('checks titles and canvas', () => {
    const d = fixtureDoc();
    expect(V.titleExists(d, 'Launch Day', 0, 90)).toEqual([]);
    expect(msgs(V.titleExists(d, 'Launch Day', 30, 90))).toContain('starts at');
    expect(V.titleExists(d, 'Other', 0, 90)).not.toEqual([]);
    expect(V.canvasIs(d, '16:9')).toEqual([]);
    expect(V.canvasIs(d, '9:16')).not.toEqual([]);
  });

  it('checks caption coverage and where the cards sit', () => {
    const d = fixtureDoc();
    expect(msgs(V.captionCoverage(d))).toContain('no caption cards');
    const text = d.tracks.find((t) => t.kind === 'text')!.id;
    const card = (id: string, start: number, dur: number) =>
      createItem('caption', { id, trackId: text, startFrame: start, durationFrames: dur, props: { words: [], style: { fontFamily: 'x', fontSize: 70, color: '#fff' }, mode: 'phrase', maxWordsPerCard: 4 } } as never);
    d.items.push(card('c1', 0, 100));
    expect(msgs(V.captionCoverage(d))).toContain('cover only');
    d.items.push(card('c2', 100, 440));
    expect(V.captionCoverage(d)).toEqual([]);
    d.items.push(card('c3', 540, 100));
    expect(msgs(V.captionCoverage(d))).toContain('past the end');
  });

  it('checks pauses and fillers against the source ranges', () => {
    const d = fixtureDoc();
    // broll-a plays source 0-6 s; a 2 s "silence" at 1-3 s is entirely still there
    expect(msgs(V.pausesGone(d, { ast_a: [{ startMs: 1000, endMs: 3000 }] }, 0.5))).toContain('1 pause(s) longer than 0.5s');
    expect(V.pausesGone(d, { ast_a: [{ startMs: 1000, endMs: 1400 }] }, 0.5)).toEqual([]);
    expect(V.fillersGone(d, 'ast_a', [[1, 1.5]])).not.toEqual([]);
    d.items.find((i) => i.id === 'itm_a')!.sourceInFrame = 60; // now plays 2 s onwards
    expect(V.fillersGone(d, 'ast_a', [[1, 1.5]])).toEqual([]);
  });

  it('checks music placement and volume', () => {
    const d = fixtureDoc();
    expect(V.musicPlaced(d, 'itm_m', 540, 0.3)).toEqual([]);
    expect(msgs(V.musicPlaced(d, 'itm_m', 540, 0.2))).toContain('volume is 0.3');
    expect(V.hasVolumeEnvelope(d, 'itm_m')).toBe(false);
  });
});
