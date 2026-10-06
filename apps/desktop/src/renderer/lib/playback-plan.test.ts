import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import { latestOnly, needsResync, planAudio, planVideo } from './playback-plan.ts';

function splitDoc() {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 });
  const track = doc0.tracks.find((t) => t.kind === 'video')!;
  const clip = createItem('video', { id: newId('itm'), trackId: track.id, startFrame: 0, durationFrames: 120, assetId: 'ast_0000000000a', sourceInFrame: 0 } as never);
  const second = newId('itm');
  const doc = applyOps(doc0, [
    { type: 'item.add', item: clip },
    { type: 'item.split', itemId: clip.id, atFrame: 60, newItemId: second },
  ]).doc;
  return { doc, first: clip.id, second };
}

describe('planAudio', () => {
  it('follows whichever half of a split clip is active and never silences the shared asset', () => {
    const { doc, first, second } = splitDoc();
    expect(planAudio(doc, 10).get('ast_0000000000a')?.id).toBe(first);
    expect(planAudio(doc, 90).get('ast_0000000000a')?.id).toBe(second);
  });

  it('is silent outside every item and for muted items or tracks', () => {
    const { doc, first } = splitDoc();
    expect(planAudio(doc, 500).get('ast_0000000000a')).toBeNull();
    const muted = applyOps(doc, [{ type: 'item.update', itemId: first, patch: { muted: true } }]).doc;
    expect(planAudio(muted, 10).get('ast_0000000000a')).toBeNull();
    const trackMuted = applyOps(doc, [{ type: 'track.update', trackId: doc.items[0]!.trackId, patch: { muted: true } }]).doc;
    expect(planAudio(trackMuted, 10).get('ast_0000000000a')).toBeNull();
  });
});

describe('planVideo', () => {
  it('picks the active video item per asset', () => {
    const { doc, second } = splitDoc();
    expect(planVideo(doc, 70).get('ast_0000000000a')?.id).toBe(second);
    expect(planVideo(doc, 500).get('ast_0000000000a')).toBeNull();
  });
});

describe('needsResync', () => {
  it('tolerates small drift but catches a jump', () => {
    expect(needsResync(10.1, 10.0)).toBe(false);
    expect(needsResync(14, 10.0)).toBe(true);
  });
});

describe('latestOnly', () => {
  it('never overlaps jobs and always ends on the newest request', async () => {
    const seen: number[] = [];
    let concurrent = 0;
    let maxConcurrent = 0;
    const run = latestOnly(async (n: number) => {
      concurrent++;
      maxConcurrent = Math.max(maxConcurrent, concurrent);
      await new Promise((r) => setTimeout(r, 5));
      seen.push(n);
      concurrent--;
    });
    await Promise.all([run(1), run(2), run(3), run(4)]);
    await new Promise((r) => setTimeout(r, 30));
    expect(maxConcurrent).toBe(1);
    expect(seen[0]).toBe(1);
    expect(seen[seen.length - 1]).toBe(4);
    expect(seen).not.toContain(2);
  });
});
