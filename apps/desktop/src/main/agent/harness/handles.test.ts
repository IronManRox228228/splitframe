import { describe, expect, it } from 'vitest';
import { buildHandles, resolveAsset, resolveClip, resolveClips } from './handles.ts';
import { fixtureSnapshot } from './test-fixtures.ts';

describe('handles', () => {
  const snap = fixtureSnapshot();
  const entries = buildHandles(snap);

  it('numbers clips per track in time order', () => {
    expect(entries.map((e) => e.handle)).toEqual(['T1·1', 'V1·1', 'V1·2', 'V1·3', 'A1·1']);
    expect(entries.find((e) => e.handle === 'V1·2')!.name).toBe('broll-b.mp4');
  });

  it('resolves handles in several spellings', () => {
    for (const ref of ['V1·2', 'v1.2', 'V1-2', 'v1 2']) {
      const r = resolveClip(entries, snap.editor, ref);
      expect(r.ok && r.value.handle).toBe('V1·2');
    }
  });

  it('resolves names with or without extension, and by substring', () => {
    for (const ref of ['broll-c', 'broll-c.mp4', 'BROLL_C']) {
      const r = resolveClip(entries, snap.editor, ref);
      expect(r.ok && r.value.handle).toBe('V1·3');
    }
    const title = resolveClip(entries, snap.editor, 'Launch');
    expect(title.ok && title.value.handle).toBe('T1·1');
  });

  it('resolves the selection', () => {
    const r = resolveClip(entries, snap.editor, 'the selected clip');
    expect(r.ok && r.value.handle).toBe('V1·2');
    const none = resolveClip(entries, { ...snap.editor, selection: [] }, 'selected');
    expect(none.ok).toBe(false);
  });

  it('lists the valid handles on unknown refs', () => {
    const r = resolveClip(entries, snap.editor, 'V9·1');
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.error).toContain('V1·1 "broll-a.mp4"');
    const r2 = resolveClip(entries, snap.editor, 'zebra');
    expect(!r2.ok && r2.error).toContain('Valid clips');
  });

  it('reports ambiguous names', () => {
    const r = resolveClip(entries, snap.editor, 'broll');
    expect(!r.ok && r.error).toContain('several');
  });

  it('resolves lists and assets', () => {
    const r = resolveClips(entries, snap.editor, ['broll-a', 'selected']);
    expect(r.ok && r.value.map((e) => e.handle)).toEqual(['V1·1', 'V1·2']);
    const a = resolveAsset(snap.assets, 'the music');
    expect(a.ok && a.value.originalName).toBe('music.mp3');
    const b = resolveAsset(snap.assets, 'interview');
    expect(b.ok && b.value.originalName).toBe('interview.mp4');
    expect(resolveAsset(snap.assets, 'nope').ok).toBe(false);
  });
});
