import { createItem } from '@cutboard/editor-core';
import { describe, expect, it } from 'vitest';
import { FACADE } from './facade.ts';
import { FakeBackend, fixtureSnapshot, speechTranscript } from './test-fixtures.ts';
import * as V from './verify.ts';

const run = (b: FakeBackend, name: string, args: Record<string, unknown>) => FACADE[name]!.run(args, b);
const item = (b: FakeBackend, id: string) => b.doc.items.find((i) => i.id === id)!;

describe('clip tools', () => {
  it('trimClip keeps the first 4 s and verifies the result', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'trimClip', { clip: 'broll-a', from: 0, to: 4 });
    expect(r.ok).toBe(true);
    expect(item(b, 'itm_a').durationFrames).toBe(120);
    expect(item(b, 'itm_b').startFrame).toBe(120); // ripple closed the gap
    expect(r.summary).toContain('V1·1 "broll-a.mp4" now plays source 0:00–0:04 (4s)');
    expect(r.summary).toContain('length stays 0:18'); // the music bed sets the length
  });

  it('trimClip clamps to the source and rejects empty ranges, naming valid handles on bad refs', async () => {
    const b = new FakeBackend();
    expect((await run(b, 'trimClip', { clip: 'nope', from: 0, to: 1 })).summary).toContain('Valid clips: T1·1');
    expect((await run(b, 'trimClip', { clip: 'V1·1', from: 5, to: 5 })).ok).toBe(false);
    const r = await run(b, 'trimClip', { clip: 'V1·1', from: '0:01', to: '0:03' });
    expect(r.ok).toBe(true);
    expect(item(b, 'itm_a').sourceInFrame).toBe(30);
  });

  it('moveClip puts C before A with no gaps or overlaps', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'moveClip', { clip: 'broll-c', before: 'broll-a' });
    expect(r.ok).toBe(true);
    expect(r.summary).toContain('broll-c.mp4 > broll-a.mp4 > broll-b.mp4');
    expect([item(b, 'itm_c').startFrame, item(b, 'itm_a').startFrame, item(b, 'itm_b').startFrame]).toEqual([0, 180, 360]);
  });

  it('moveClip onto other clips inserts instead of stacking', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'moveClip', { clip: 'V1·2', toSec: 0 });
    expect(r.ok).toBe(true);
    expect([item(b, 'itm_b').startFrame, item(b, 'itm_a').startFrame, item(b, 'itm_c').startFrame]).toEqual([0, 180, 360]);
  });

  it('deleteClips deletes the selection and closes the gap', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'deleteClips', { clips: ['selected'] });
    expect(r.ok).toBe(true);
    expect(b.doc.items.some((i) => i.id === 'itm_b')).toBe(false);
    expect(item(b, 'itm_c').startFrame).toBe(180);
    const none = new FakeBackend();
    none.selection = [];
    expect((await run(none, 'deleteClips', { clips: ['selected'] })).ok).toBe(false);
  });

  it('splitClip refuses a time outside the clip and splits inside it', async () => {
    const b = new FakeBackend();
    expect((await run(b, 'splitClip', { clip: 'V1·1', atSec: 9 })).ok).toBe(false);
    const r = await run(b, 'splitClip', { clip: 'V1·1', atSec: 2 });
    expect(r.ok).toBe(true);
    expect(b.doc.items.filter((i) => i.assetId === 'ast_a')).toHaveLength(2);
  });

  it('setClipProps changes speed and keeps the content', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'setClipProps', { clip: 'V1·1', speed: 2, volume: 0.5 });
    expect(r.ok).toBe(true);
    expect(item(b, 'itm_a').durationFrames).toBe(90);
    expect(item(b, 'itm_a').volume).toBe(0.5);
  });
});

describe('speed changes ripple', () => {
  const starts = (b: FakeBackend) => ['itm_a', 'itm_b', 'itm_c'].map((id) => item(b, id).startFrame);

  it('slowing a clip pushes the clips after it right, so nothing overlaps', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'setClipProps', { clip: 'V1·2', speed: 0.5 });
    expect(r.ok).toBe(true);
    expect(item(b, 'itm_b').durationFrames).toBe(360);
    expect(starts(b)).toEqual([0, 180, 540]);
  });

  it('speeding one up closes the gap behind it', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'setClipProps', { clip: 'V1·2', speed: 2 });
    expect(r.ok).toBe(true);
    expect(starts(b)).toEqual([0, 180, 270]);
  });
});

describe('titles, canvas, export, undo', () => {
  it('addTitle supplies defaults: text track, 0 s, 3 s', async () => {
    const b = new FakeBackend(fixtureSnapshot());
    b.doc.items = b.doc.items.filter((i) => i.type !== 'text');
    const r = await run(b, 'addTitle', { text: 'Launch Day' });
    expect(r.ok).toBe(true);
    const t = b.doc.items.find((i) => i.type === 'text')!;
    expect([t.startFrame, t.durationFrames]).toEqual([0, 90]);
    expect(b.doc.tracks.find((x) => x.id === t.trackId)!.kind).toBe('text');
  });

  it('addTitle does not add the same title twice', async () => {
    const b = new FakeBackend(fixtureSnapshot());
    b.doc.items = b.doc.items.filter((i) => i.type !== 'text');
    await run(b, 'addTitle', { text: 'Launch Day' });
    const again = await run(b, 'addTitle', { text: 'Launch Day' });
    expect(again.ok).toBe(true);
    expect(again.mutated).toBe(false);
    expect(b.doc.items.filter((i) => i.type === 'text')).toHaveLength(1);
  });

  it('setCanvas switches to 9:16 and verifies', async () => {
    const b = new FakeBackend();
    const r = await run(b, 'setCanvas', { aspect: '9:16' });
    expect(r.ok).toBe(true);
    expect([b.doc.project.width, b.doc.project.height]).toEqual([1080, 1920]);
  });

  it('exportVideo reports a started export', async () => {
    const r = await run(new FakeBackend(), 'exportVideo', { preset: '720p' });
    expect(r.ok).toBe(true);
    expect(r.summary).toContain('export started (720p)');
  });

  it('undo reverts the last change and says what it undid', async () => {
    const b = new FakeBackend();
    await run(b, 'deleteClips', { clips: ['V1·1'] });
    const r = await run(b, 'undo', { steps: 1 });
    expect(r.ok).toBe(true);
    expect(b.doc.items.some((i) => i.id === 'itm_a')).toBe(true);
    expect((await run(new FakeBackend(), 'undo', {})).summary).toContain('nothing to undo');
  });
});

describe('speech tools', () => {
  const sentences = ['Hello there and welcome to the show.', 'We make video tools for everyone.', 'Um, so that is the plan.', 'Thanks for listening to us today.'];
  const setup = () => {
    const snap = fixtureSnapshot({ transcripts: [speechTranscript('ast_int', sentences, 0, 1.2)] });
    snap.doc.items = [
      createItem('video', { id: 'itm_i', trackId: snap.doc.tracks.find((t) => t.kind === 'video')!.id, startFrame: 0, durationFrames: 20 * 30, assetId: 'ast_int', sourceInFrame: 0 } as never),
    ];
    snap.assets = snap.assets.map((a) => (a.id === 'ast_int' ? { ...a, durationMs: 20000 } : a));
    const b = new FakeBackend(snap);
    // silences sit between the sentences
    const t = speechTranscript('ast_int', sentences, 0, 1.2).words;
    const ends = t.filter((w) => /\.$/.test(w.w)).map((w) => w.endMs);
    b.silenceMaps['ast_int'] = ends.map((e) => ({ startMs: e, endMs: e + 1200 })).slice(0, 3);
    return b;
  };

  it('removePauses cuts the silences and the verifier confirms', async () => {
    const b = setup();
    const r = await run(b, 'removePauses', { minPauseSec: 0.5 });
    expect(r.ok).toBe(true);
    expect(r.summary).toContain('pauses longer than 0.5s');
    expect(V.docLengthFrames(b.doc)).toBeLessThan(20 * 30 - 60);
  });

  it('removePauses reports clearly when there is nothing to cut', async () => {
    const b = setup();
    const r = await run(b, 'removePauses', { minPauseSec: 5 });
    expect(r.ok).toBe(true);
    expect(r.mutated).toBe(false);
    expect(r.summary).toContain('no pauses longer than 5s');
  });

  it('removeFillers cuts the um and the verifier confirms it is gone', async () => {
    const b = setup();
    const before = V.docLengthFrames(b.doc);
    const r = await run(b, 'removeFillers', {});
    expect(r.ok).toBe(true);
    expect(r.summary).toContain('cut 1 filler word');
    expect(V.docLengthFrames(b.doc)).toBeLessThan(before);
  });

  it('addCaptions replaces old cards and styleCaptions scales them in one step', async () => {
    const b = setup();
    const r1 = await run(b, 'addCaptions', {});
    expect(r1.ok).toBe(true);
    const n = b.doc.items.filter((i) => i.type === 'caption').length;
    expect(n).toBeGreaterThan(3);
    const r2 = await run(b, 'addCaptions', {});
    expect(r2.summary).toContain('replacing');
    expect(b.doc.items.filter((i) => i.type === 'caption')).toHaveLength(n);
    const size = V.captionFontSize(b.doc)!;
    const r3 = await run(b, 'styleCaptions', { sizeFactor: 1.5 });
    expect(r3.ok).toBe(true);
    expect(V.captionFontSize(b.doc)).toBe(Math.round(size * 1.5));
  });

  it('styleCaptions without captions is a clear error', async () => {
    expect((await run(new FakeBackend(), 'styleCaptions', {})).summary).toContain('no captions');
  });

  it('addMusic places the track quietly under the video and ducks under the speech', async () => {
    const b = setup();
    b.doc.items = b.doc.items.filter((i) => i.type !== 'audio');
    const r = await run(b, 'addMusic', { asset: 'music', volume: 0.2 });
    expect(r.ok).toBe(true);
    const m = b.doc.items.find((i) => i.type === 'audio')!;
    expect(m.startFrame).toBe(0);
    expect(m.volume).toBe(0.2);
    expect(m.durationFrames).toBe(20 * 30); // covers the 20 s video
    expect(r.summary).toContain('dips under');
    expect(V.hasVolumeEnvelope(b.doc, m.id)).toBe(true);
  });
});

describe('judgment-step tools', () => {
  it('assembleSequence builds the cut and readTranscript returns numbered sentences', async () => {
    const snap = fixtureSnapshot({ transcripts: [speechTranscript('ast_int', ['One two three four.', 'Five six seven eight.'], 0, 1)] });
    const b = new FakeBackend(snap);
    const read = await run(b, 'readTranscript', { asset: 'interview' });
    expect(read.summary).toContain('[2] 0:02.6–0:04.2 Five six seven eight.');
    const r = await run(b, 'assembleSequence', { ranges: [{ asset: 'interview.mp4', fromSec: 2.6, toSec: 4.2 }, { asset: 'interview.mp4', fromSec: 0, toSec: 1.6 }] });
    expect(r.ok).toBe(true);
    const added = b.doc.items.filter((i) => i.assetId === 'ast_int');
    expect(added.map((i) => i.sourceInFrame)).toEqual([78, 0]);
    const found = await run(b, 'searchTranscript', { phrase: 'six seven' });
    expect(found.summary).toContain('interview.mp4');
  });
});
