import { describe, expect, it } from 'vitest';
import { FACADE } from './facade.ts';
import { diffCounts, docDiff, lengthChange, previewLine, previewTool } from './preview.ts';
import { FakeBackend, fixtureDoc } from './test-fixtures.ts';

describe('docDiff', () => {
  it('an untouched document has no changes', () => {
    const d = docDiff(fixtureDoc(), fixtureDoc());
    expect(d).toMatchObject({ added: 0, removed: 0, trimmed: 0, moved: 0, changed: 0, affectedIds: [], beforeSec: 18, afterSec: 18 });
  });

  it('counts removed, trimmed, moved and changed clips and the new length', () => {
    const before = fixtureDoc();
    const after = fixtureDoc();
    after.items = after.items.filter((i) => i.id !== 'itm_b'); // removed
    after.items.find((i) => i.id === 'itm_a')!.durationFrames = 90; // trimmed
    after.items.find((i) => i.id === 'itm_c')!.startFrame = 90; // moved
    after.items.find((i) => i.id === 'itm_m')!.volume = 0.9; // changed
    const d = docDiff(before, after);
    expect([d.removed, d.trimmed, d.moved, d.changed]).toEqual([1, 1, 1, 1]);
    expect(d.clipsGone).toBe(1);
    expect(d.clipsTouched).toBe(2);
    expect(d.affectedIds.sort()).toEqual(['itm_a', 'itm_b', 'itm_c', 'itm_m']);
    expect(d.afterSec).toBeCloseTo(18, 5); // the music still ends at 18 s
  });

  it('captions are counted apart from clips', () => {
    const before = fixtureDoc();
    const after = fixtureDoc();
    after.items.push({ ...after.items[0]!, id: 'cap1', type: 'caption' } as never);
    const d = docDiff(before, after);
    expect(d).toMatchObject({ added: 0, captionsAdded: 1 });
    expect(diffCounts(d)).toBe('1 caption added');
  });

  it('formats the length move and a one-line preview', () => {
    const a = fixtureDoc();
    a.items = a.items.filter((i) => i.id !== 'itm_c' && i.id !== 'itm_m');
    const d = docDiff(fixtureDoc(), a);
    expect(lengthChange(d)).toBe('0:18 → 0:12');
    expect(previewLine({ ok: true, summary: 'cut 6s of pauses longer than 0.5s; length 0:18 -> 0:12' }, d)).toBe('Cut 6s of pauses longer than 0.5s · 0:18 → 0:12');
  });
});

describe('previewTool (dry run)', () => {
  it('computes the effect of a tool without touching the project', async () => {
    const b = new FakeBackend();
    const before = JSON.stringify(b.doc);
    const p = await previewTool(FACADE['deleteClips']!, { clips: ['V1·2'] }, b);
    expect(p.out.ok).toBe(true);
    expect(p.diff.removed).toBe(1);
    expect(p.diff.affectedIds).toContain('itm_b');
    expect(JSON.stringify(b.doc)).toBe(before);
    expect(b.history).toHaveLength(0);
  });

  it('previews tools that go through the classic registry', async () => {
    const b = new FakeBackend();
    const p = await previewTool(FACADE['addTitle']!, { text: 'Hi', atSec: 0, durationSec: 2 }, b);
    expect(p.out.ok).toBe(true);
    expect(p.diff.added).toBe(1);
    expect(b.doc.items.filter((i) => i.type === 'text')).toHaveLength(1); // only the fixture's own title
    expect(b.calls).toHaveLength(0);
  });

  it('a failing step is reported on the copy, not thrown', async () => {
    const b = new FakeBackend();
    const p = await previewTool(FACADE['deleteClips']!, { clips: ['nope'] }, b);
    expect(p.out.ok).toBe(false);
    expect(p.diff.removed).toBe(0);
  });
});

describe('rebuilt clips', () => {
  it('a clip re-created under a new id with the same footage is not gone', () => {
    const before = fixtureDoc();
    const after = fixtureDoc();
    const a = after.items.find((i) => i.id === 'itm_a')!;
    after.items = after.items.filter((i) => i.id !== 'itm_a');
    after.items.push({ ...a, id: 'itm_a2' });
    expect(docDiff(before, after).clipsGone).toBe(0);
  });

  it('a clip whose footage is no longer played anywhere is gone', () => {
    const before = fixtureDoc();
    const after = fixtureDoc();
    after.items = after.items.filter((i) => i.id !== 'itm_a');
    expect(docDiff(before, after).clipsGone).toBe(1);
  });
});
