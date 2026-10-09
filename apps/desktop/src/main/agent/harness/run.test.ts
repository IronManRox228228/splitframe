import { describe, expect, it } from 'vitest';
import type { Llm, ToolCallOut } from './llm.ts';
import { composeReply, mechanicalCall, runHarness, runStoredPlan } from './run.ts';
import { FakeBackend, fixtureSnapshot, speechTranscript } from './test-fixtures.ts';

/** A scripted model: each call pops the next scripted answer, so tests also prove calls are sequential. */
function script(answers: { tools?: ToolCallOut[]; json?: unknown; text?: string }[]) {
  const seen: string[] = [];
  let busy = false;
  const next = async (kind: string, prompt: string) => {
    expect(busy).toBe(false);
    busy = true;
    await Promise.resolve();
    busy = false;
    seen.push(`${kind}:${prompt}`);
    const a = answers.shift();
    if (!a) throw new Error(`unexpected ${kind} call`);
    return a;
  };
  const llm: Llm = {
    async json({ prompt }) {
      return { value: (await next('json', prompt)).json, usage: {} };
    },
    async tools({ prompt }) {
      const a = await next('tools', prompt);
      return { text: a.text ?? '', calls: a.tools ?? [], usage: {} };
    },
    async text({ prompt }) {
      return { text: (await next('text', prompt)).text ?? '', usage: {} };
    },
  };
  return { llm, seen, left: () => answers.length };
}

const sink = () => {
  const events: { type: string; payload: Record<string, unknown> }[] = [];
  return { events, emit: (type: string, payload: Record<string, unknown>) => void events.push({ type, payload }) } as const;
};

describe('harness run', () => {
  it('single-step request: router by rules, one executor call, verified reply, state doc in the prompt', async () => {
    const b = new FakeBackend();
    const m = script([{ tools: [{ name: 'trimClip', args: { clip: 'broll-c', from: 0, to: 4 } }] }]);
    const s = sink();
    const r = await runHarness('Trim broll-c so that only its first 4 seconds play.', [], { backend: b, planner: m.llm, executor: m.llm, sink: s as never });
    expect(b.doc.items.find((i) => i.id === 'itm_c')!.durationFrames).toBe(120);
    expect(m.seen[0]).toContain('V1·3 "broll-c.mp4"');
    expect(m.seen[0]).not.toContain('itm_c');
    expect(r.reply).toContain('now plays source 0:00–0:04');
    expect(s.events.filter((e) => e.type === 'chat:tool').map((e) => e.payload['phase'])).toEqual(['call', 'result']);
    expect(b.groups).toHaveLength(1);
    expect(b.openGroup).toBe(false);
  });

  it('retries with the specific error when the model uses a bad clip reference', async () => {
    const b = new FakeBackend();
    const m = script([
      { tools: [{ name: 'deleteClips', args: { clips: ['the green one'] } }] },
      { tools: [{ name: 'deleteClips', args: { clips: ['V1·2'] } }] },
    ]);
    const r = await runHarness('Delete the second clip', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never });
    expect(m.seen[1]).toContain('FAILED deleteClips: Unknown clip "the green one". Valid clips');
    expect(b.doc.items.some((i) => i.id === 'itm_b')).toBe(false);
    expect(r.reply).toContain('Deleted V1·2');
    expect(r.reply).not.toContain('could not');
  });

  it('acts instead of asking: a text-only answer gets one nudge', async () => {
    const b = new FakeBackend();
    b.doc.items = b.doc.items.filter((i) => i.type !== 'text');
    const m = script([{ text: 'What should the title say and how long?' }, { tools: [{ name: 'addTitle', args: { text: 'Launch Day' } }] }]);
    await runHarness('Add a title that says "Launch Day"', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never });
    expect(m.seen[1]).toContain('do not ask');
    expect(b.doc.items.find((i) => i.type === 'text')!.durationFrames).toBe(90);
  });

  it('a request naming two actions gets a follow-up round for the rest', async () => {
    const b = new FakeBackend();
    const m = script([
      { tools: [{ name: 'splitClip', args: { clip: 'V1·1', atSec: 2 } }] },
      { tools: [{ name: 'deleteClips', args: { clips: ['V1·2'] } }] },
      { text: 'DONE' },
    ]);
    await runHarness('Split the first clip at 2 seconds and delete the second half.', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never });
    expect(m.seen[1]).toContain('ONLY for the part not done yet');
    expect(b.doc.items.filter((i) => i.assetId === 'ast_a')).toHaveLength(1);
    expect(m.left()).toBe(0);
  });

  it('reports failure honestly instead of claiming success', async () => {
    const b = new FakeBackend();
    const m = script([{ tools: [{ name: 'splitClip', args: { clip: 'V1·1', atSec: 99 } }] }, { tools: [{ name: 'splitClip', args: { clip: 'V1·1', atSec: 99 } }] }, { tools: [{ name: 'splitClip', args: { clip: 'V1·1', atSec: 99 } }] }]);
    const r = await runHarness('split the first clip at 99 seconds', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never });
    expect(r.reply).toContain('I could not finish');
    expect(b.doc.items).toHaveLength(5);
  });

  it('classifies with one constrained call when no rule fires, using the recent turns', async () => {
    const b = new FakeBackend();
    const m = script([{ json: { intents: ['captions'] } }, ...[1, 2, 3].map(() => ({ tools: [{ name: 'styleCaptions', args: { sizeFactor: 1.3 } }] }))]);
    const r = await runHarness('Make them bigger.', [{ role: 'user', content: 'Add captions' }, { role: 'assistant', content: 'Added 40 caption cards' }], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never });
    expect(m.seen[0]).toContain('assistant: Added 40 caption cards');
    expect(r.reply).toContain('There are no captions yet'); // nothing to restyle: reported as a failure, not a claim
  });

  it('compound request: plan, mechanical steps without the executor, persisted statuses', async () => {
    const snap = fixtureSnapshot({ transcripts: [speechTranscript('ast_int', ['Hello there my dear friends today.', 'We make tools for everyone.'], 0, 1)] });
    snap.doc.items = snap.doc.items.filter((i) => i.type !== 'text' && i.type !== 'audio');
    const b = new FakeBackend(snap);
    b.assets = b.assets.map((a) => (a.id === 'ast_int' ? { ...a, durationMs: 8000 } : a));
    const draft = {
      summary: 'A captioned cut with music and a title',
      steps: [
        { kind: 'title', goal: 'title', params: { text: 'Launch Day', startSec: 0, durationSec: 3 }, accept: [] },
        { kind: 'music', goal: 'music', params: { asset: 'music.mp3', volume: 0.2 }, accept: [] },
        { kind: 'captions', goal: 'captions', params: {}, accept: [] },
      ],
    };
    // the video must hold speech: put the interview on the video track
    const video = b.doc.tracks.find((t) => t.kind === 'video')!.id;
    b.doc.items = [{ ...b.doc.items[0]!, id: 'itm_i', trackId: video, assetId: 'ast_int', startFrame: 0, durationFrames: 240, sourceInFrame: 0 } as never];
    const m = script([{ json: draft }]);
    const r = await runHarness('Make this ready to post: add captions, put the music quietly underneath, and add a title that says "Launch Day" for the first 3 seconds.', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never, mode: 'auto' });
    expect(m.left()).toBe(0); // no executor call: every step was mechanical
    expect(b.doc.items.some((i) => i.type === 'caption')).toBe(true);
    expect(b.doc.items.some((i) => i.type === 'audio' && i.volume === 0.2)).toBe(true);
    expect(b.doc.items.some((i) => i.type === 'text')).toBe(true);
    const stored = b.plan as { steps: { status: string }[] };
    expect(stored.steps.map((s) => s.status)).toEqual(['done', 'done', 'done']);
    expect(r.reply).toContain('Done (3/3 steps)');
    expect(b.groups).toHaveLength(1);
  });

  it('judgment step: the model picks sentence numbers, code builds the cut to length', async () => {
    const sentences = ['Intro words here now.', 'The bakery story starts here today.', 'Maria opened her shop in spring.', 'Sales doubled in a month overall.', 'Now back to pricing details.'];
    const snap = fixtureSnapshot({ transcripts: [speechTranscript('ast_int', sentences, 0, 0.5)] });
    snap.assets = snap.assets.map((a) => (a.id === 'ast_int' ? { ...a, durationMs: 20000 } : a));
    snap.doc.items = [{ ...snap.doc.items[0]!, id: 'itm_i', assetId: 'ast_int', startFrame: 0, durationFrames: 600, sourceInFrame: 0 } as never];
    const b = new FakeBackend(snap);
    const plan = { summary: 'A short teaser', steps: [{ kind: 'assemble', goal: 'bakery teaser', params: { topic: 'bakery story', targetSec: 6 }, accept: [] }] };
    const m = script([{ json: plan }, { json: { picks: [{ first_sentence: 2, last_sentence: 4 }] } }]);
    const r = await runHarness('Make a 6-second teaser about the bakery story only.', [], { backend: b, planner: m.llm, executor: m.llm, sink: sink() as never, mode: 'auto', confirm: async () => 'apply' });
    expect(m.seen[1]).toContain('[2] ');
    const items = b.doc.items.filter((i) => i.assetId === 'ast_int');
    const total = items.reduce((a, i) => a + i.durationFrames, 0) / 30;
    expect(total).toBeGreaterThan(3);
    expect(total).toBeLessThan(8);
    expect(items[0]!.sourceInFrame).toBeGreaterThan(60); // starts at the bakery sentence, not the intro
    expect(r.reply).toContain('Built a');
  });
});

describe('plan to calls and replies', () => {
  it('maps mechanical kinds to facade tools with defaults', () => {
    const step = (kind: string, params = {}) => ({ id: 1, kind, goal: '', params, accept: [], status: 'pending' }) as never;
    expect(mechanicalCall(step('remove_pauses'))).toEqual({ tool: 'removePauses', args: { minPauseSec: 0.5 } });
    expect(mechanicalCall(step('title', { text: 'Hi' }))).toEqual({ tool: 'addTitle', args: { text: 'Hi', atSec: 0, durationSec: 3 } });
    expect(mechanicalCall(step('export'))!.args).toEqual({ preset: '1080p' });
    expect(mechanicalCall(step('assemble'))).toBeNull();
  });

  it('builds the reply from verified records only', () => {
    expect(composeReply([{ label: 'a', tool: 'x', ok: true, summary: 'cut 2s of pauses', mutated: true }], '', null)).toBe('- Cut 2s of pauses'.slice(2) + '.');
    const mixed = composeReply(
      [
        { label: 'a', tool: 'x', ok: false, summary: 'no clip', mutated: false },
        { label: 'a', tool: 'x', ok: true, summary: 'fixed it', mutated: true },
        { label: 'b', tool: 'y', ok: false, summary: 'nothing found', mutated: false },
      ],
      '',
      null,
    );
    expect(mixed).toContain('Fixed it');
    expect(mixed).toContain('I could not finish "b": nothing found');
    expect(mixed).not.toContain('no clip');
  });
});

describe('agent modes in a run', () => {
  const run = (msg: string, b: FakeBackend, m: ReturnType<typeof script>, extra: Partial<Parameters<typeof runHarness>[2]> = {}) => {
    const s = sink();
    return runHarness(msg, [], { backend: b, planner: m.llm, executor: m.llm, sink: s as never, ...extra }).then((r) => ({ r, s }));
  };
  const trim = { tools: [{ name: 'trimClip', args: { clip: 'broll-c', from: 0, to: 4 } }] };
  const clipC = (b: FakeBackend) => b.doc.items.find((i) => i.id === 'itm_c')!.durationFrames;

  it('Default applies a small single-step request immediately, with no confirmation', async () => {
    const b = new FakeBackend();
    const asked: unknown[] = [];
    const { r } = await run('Trim broll-c so that only its first 4 seconds play.', b, script([trim]), { mode: 'default', confirm: async (q) => (asked.push(q), 'apply') });
    expect(clipC(b)).toBe(120);
    expect(asked).toHaveLength(0);
    expect(r.confirmations).toBeUndefined();
  });

  it('Ask shows a preview first; Skip changes nothing, Apply applies', async () => {
    const b = new FakeBackend();
    const asked: { line: string; affectedIds: string[] }[] = [];
    const skip = await run('Trim broll-c so that only its first 4 seconds play.', b, script([trim]), { mode: 'ask', confirm: async (q) => (asked.push(q), 'skip') });
    expect(clipC(b)).toBe(180);
    expect(asked).toHaveLength(1);
    expect(asked[0]!.line).toContain('broll-c');
    expect(asked[0]!.affectedIds).toContain('itm_c');
    expect(skip.r.reply).toContain('Skipped');
    expect(skip.s.events.some((e) => e.type === 'chat:confirm' && e.payload['phase'] === 'ask')).toBe(true);
    expect(b.history).toHaveLength(0);

    const ok = await run('Trim broll-c so that only its first 4 seconds play.', b, script([trim]), { mode: 'ask', confirm: async () => 'apply' });
    expect(clipC(b)).toBe(120);
    expect(ok.r.confirmations).toEqual({ asked: 1, applied: 1 });
  });

  it('with no way to answer, a confirmation counts as Skip (never a silent edit)', async () => {
    const b = new FakeBackend();
    await run('Trim broll-c so that only its first 4 seconds play.', b, script([trim]), { mode: 'ask' });
    expect(clipC(b)).toBe(180);
  });

  it('the destructive guard asks even in Auto, and Skip keeps the timeline', async () => {
    const b = new FakeBackend();
    const del = { tools: [{ name: 'deleteClips', args: { clips: ['V1·1', 'V1·2', 'V1·3'] } }] };
    const asked: { reasons: string[] }[] = [];
    const { r } = await run('Delete all three clips', b, script([del]), { mode: 'auto', confirm: async (q) => (asked.push(q), 'skip') });
    expect(asked).toHaveLength(1);
    expect(asked[0]!.reasons[0]).toContain('most of the timeline');
    expect(b.doc.items.filter((i) => i.type === 'video')).toHaveLength(3);
    expect(r.reply).toContain('Skipped');
  });

  it('Plan mode: a single request becomes a plan card, nothing changes, Run executes it', async () => {
    const b = new FakeBackend();
    const draft = { summary: 'Trim the last clip', steps: [{ kind: 'clip_edit', goal: 'keep the first 4 s of broll-c', params: {}, accept: [] }] };
    const m = script([{ json: draft }]);
    const before = JSON.stringify(b.doc);
    const { r, s } = await run('Trim broll-c so that only its first 4 seconds play.', b, m, { mode: 'plan' });
    expect(r.waiting).toBe(true);
    expect(JSON.stringify(b.doc)).toBe(before);
    expect(b.groups).toHaveLength(0);
    const card = s.events.filter((e) => e.type === 'chat:plan').pop()!.payload as { state: string; steps: { status: string }[] };
    expect(card.state).toBe('proposed');
    expect(card.steps[0]!.status).toBe('pending');
    expect(b.plan).not.toBeNull();

    const m2 = script([trim]);
    const s2 = sink();
    const done = await runStoredPlan({ backend: b, planner: m2.llm, executor: m2.llm, sink: s2 as never, mode: 'plan' });
    expect(clipC(b)).toBe(120);
    expect(done.reply).toContain('Done (1/1 steps)');
    expect((s2.events.filter((e) => e.type === 'chat:plan').pop()!.payload as { state: string }).state).toBe('done');
  });

  it('Plan mode refuses undo and export instead of doing them', async () => {
    const b = new FakeBackend();
    const { r } = await run('Undo that', b, script([]), { mode: 'plan' });
    expect(r.reply).toContain('Plan mode');
    expect(b.calls).toHaveLength(0);
  });

  it('Default shows the plan card for a multi-step job and waits; Auto runs it', async () => {
    const draft = {
      summary: 'Title and a square canvas',
      steps: [
        { kind: 'title', goal: 'title', params: { text: 'Summer Sale', startSec: 0, durationSec: 3 }, accept: [] },
        { kind: 'canvas', goal: 'square', params: { aspect: '1:1' }, accept: [] },
      ],
    };
    const msg = 'Add a title that says "Summer Sale" and make the canvas square.';
    const b = new FakeBackend();
    const { r, s } = await run(msg, b, script([{ json: draft }]), { mode: 'default' });
    expect(r.waiting).toBe(true);
    expect(b.doc.items.filter((i) => i.type === 'text')).toHaveLength(1);
    const card = s.events.filter((e) => e.type === 'chat:plan').pop()!.payload as { steps: { preview?: string }[] };
    expect(card.steps.map((x) => x.preview)).toHaveLength(2);
    expect(card.steps.map((x) => x.preview).join(' | ')).toContain('Summer Sale');

    const b2 = new FakeBackend();
    const auto = await run(msg, b2, script([{ json: draft }]), { mode: 'auto' });
    expect(auto.r.waiting).toBeUndefined();
    expect(b2.doc.items.filter((i) => i.type === 'text')).toHaveLength(2);
  });

  it('a revision goes to the planner and replaces the stored plan', async () => {
    const b = new FakeBackend();
    const first = { summary: 'Title', steps: [{ kind: 'title', goal: 'title', params: { text: 'Launch Day' }, accept: [] }] };
    await run('Add a title that says "Launch Day"', b, script([{ json: first }]), { mode: 'plan' });
    const second = { summary: 'Title and square', steps: [{ kind: 'title', goal: 'title', params: { text: 'Launch Day' }, accept: [] }, { kind: 'canvas', goal: 'square', params: { aspect: '1:1' }, accept: [] }] };
    const m = script([{ json: second }]);
    const { r } = await run('also make it square', b, m, { mode: 'plan', revise: true });
    expect(m.seen[0]).toContain('CURRENT PLAN');
    expect(r.waiting).toBe(true);
    expect((b.plan as { steps: unknown[] }).steps).toHaveLength(2);
  });
});
