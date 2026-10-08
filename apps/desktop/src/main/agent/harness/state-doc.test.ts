import { describe, expect, it } from 'vitest';
import { renderStateDoc } from './state-doc.ts';
import { fixtureSnapshot, speechTranscript } from './test-fixtures.ts';
import type { EditPlan } from './plan.ts';

describe('state doc', () => {
  it('renders handles, seconds, selection and assets, with no ids or frame numbers', () => {
    const snap = fixtureSnapshot({ transcripts: [speechTranscript('ast_int', ['Hi everyone thanks for having me', 'Second sentence here'])] });
    const doc = renderStateDoc({ snap, notes: (id) => (id === 'ast_a' ? { scenes: 3, issues: ['shaky'], text: ['SALE'] } : null) });
    expect(doc).toContain('PROJECT: 1920x1080 (16:9) · 30 fps · length 0:18');
    expect(doc).toContain('selected: V1·2 "broll-b.mp4"');
    expect(doc).toContain('playhead 0:07');
    expect(doc).toContain('V1·1 "broll-a.mp4" 0:00–0:06 (6s) src 0:00–0:06');
    expect(doc).toContain('V1·3 "broll-c.mp4" 0:12–0:18 (6s)');
    expect(doc).toContain('T1·1 "Launch Day" 0:00–0:03 (3s)');
    expect(doc).toContain('A1·1 "music.mp3" 0:00–0:18 (18s) src 0:00–0:18 volume 0.3');
    expect(doc).toContain('"interview.mp4" video 1:00 · 1920x1080 · transcript 9 words');
    expect(doc).toContain('starts "Hi everyone thanks for having me Second sentence here..."');
    expect(doc).toContain('notes: 3 scenes; issues: shaky; on-screen text: "SALE"');
    expect(doc).not.toMatch(/itm_|ast_|frame/);
  });

  it('collapses captions into one line and shows the plan with step status', () => {
    const snap = fixtureSnapshot();
    const text = snap.doc.tracks.find((t) => t.kind === 'text')!.id;
    for (let i = 0; i < 5; i++) {
      snap.doc.items.push({
        ...snap.doc.items.find((x) => x.type === 'text')!,
        id: `cap_${i}`,
        type: 'caption',
        trackId: text,
        startFrame: i * 30,
        durationFrames: 30,
        props: { words: [], style: { fontSize: 76 }, mode: 'phrase', maxWordsPerCard: 4 },
      } as never);
    }
    const plan: EditPlan = {
      request: 'x',
      summary: 'A short cut',
      createdAt: 'now',
      steps: [
        { id: 1, kind: 'remove_pauses', goal: 'tighten', params: {}, accept: [], status: 'done', note: 'cut 2.0s' },
        { id: 2, kind: 'captions', goal: 'add captions', params: {}, accept: [], status: 'pending' },
      ],
    };
    const doc = renderStateDoc({ snap, plan });
    expect(doc).toContain('captions: 5 cards 0:00–0:05, font size 76');
    expect(doc).not.toContain('cap_');
    expect(doc).toContain('1. [x] remove_pauses: tighten (cut 2.0s)');
    expect(doc).toContain('2. [ ] captions: add captions');
  });

  it('collapses very long tracks', () => {
    const snap = fixtureSnapshot();
    const video = snap.doc.tracks.find((t) => t.kind === 'video')!.id;
    for (let i = 0; i < 60; i++) snap.doc.items.push({ ...snap.doc.items[0]!, id: `x${i}`, trackId: video, startFrame: 1000 + i * 10, durationFrames: 10 });
    const doc = renderStateDoc({ snap });
    expect(doc).toMatch(/\.\.\. \d+ more clips \.\.\./);
  });
});
