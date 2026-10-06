import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import { atempoChain, buildAudioGraph, exportMediaPath, selectAudioSources, volumeExpression } from './export-plan.ts';

function docWith(items: Record<string, unknown>[], mutedTrack?: string) {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'T', fps: 30 });
  const video = doc0.tracks.find((t) => t.kind === 'video')!;
  const audio = doc0.tracks.find((t) => t.kind === 'audio')!;
  const made = items.map((i) =>
    createItem((i['type'] as 'video' | 'audio') ?? 'video', {
      id: newId('itm'),
      trackId: i['type'] === 'audio' ? audio.id : video.id,
      sourceInFrame: 0,
      ...i,
    } as never),
  );
  let doc = applyOps(doc0, made.map((item) => ({ type: 'item.add' as const, item }))).doc;
  if (mutedTrack) doc = applyOps(doc, [{ type: 'track.update', trackId: audio.id, patch: { muted: true } }]).doc;
  return { doc, items: made };
}

describe('exportMediaPath', () => {
  const base = { path: '/media/clip.mp4', proxyPath: '/cache/clip-proxy.mp4' };

  it('renders from the original when Chromium can decode it', () => {
    expect(exportMediaPath({ kind: 'video', ...base, metadata: { codec: 'h264' } })).toBe(base.path);
    expect(exportMediaPath({ kind: 'image', path: '/media/a.png', metadata: {} })).toBe('/media/a.png');
  });

  it('falls back to the proxy for unknown or undecodable codecs and containers', () => {
    expect(exportMediaPath({ kind: 'video', ...base, metadata: {} })).toBe(base.proxyPath);
    expect(exportMediaPath({ kind: 'video', ...base, metadata: { codec: 'hevc' } })).toBe(base.proxyPath);
    expect(exportMediaPath({ kind: 'video', ...base, path: '/media/clip.mkv', metadata: { codec: 'h264' } })).toBe(base.proxyPath);
  });
});

describe('selectAudioSources', () => {
  it('skips silent files, missing assets, muted items and muted tracks', () => {
    const { doc, items } = docWith([
      { type: 'video', startFrame: 0, durationFrames: 30, assetId: 'ast_talking' },
      { type: 'video', startFrame: 30, durationFrames: 30, assetId: 'ast_silent' },
      { type: 'video', startFrame: 60, durationFrames: 30, assetId: 'ast_missing' },
      { type: 'video', startFrame: 90, durationFrames: 30, assetId: 'ast_talking', muted: true },
      { type: 'audio', startFrame: 0, durationFrames: 30, assetId: 'ast_music' },
    ]);
    const assets: Record<string, { path: string; hasAudio: boolean }> = {
      ast_talking: { path: 'a.mp4', hasAudio: true },
      ast_silent: { path: 'b.mp4', hasAudio: false },
      ast_music: { path: 'm.wav', hasAudio: true },
    };
    const picked = selectAudioSources(doc, (id) => assets[id] ?? null);
    expect(picked.map((s) => s.item.id)).toEqual([items[0]!.id, items[4]!.id]);

    const { doc: mutedDoc } = docWith([{ type: 'audio', startFrame: 0, durationFrames: 30, assetId: 'ast_music' }], 'audio');
    expect(selectAudioSources(mutedDoc, (id) => assets[id] ?? null)).toEqual([]);
  });
});

describe('buildAudioGraph', () => {
  it('returns null when there is nothing to mix', () => {
    expect(buildAudioGraph([], 30)).toBeNull();
  });

  it('numbers inputs by the sources that survived filtering and delays after the volume envelope', () => {
    const { doc, items } = docWith([
      { type: 'video', startFrame: 150, durationFrames: 90, assetId: 'ast_a', sourceInFrame: 30, props: { fadeInFrames: 15, fadeOutFrames: 0 } },
      { type: 'audio', startFrame: 0, durationFrames: 30, assetId: 'ast_b' },
    ]);
    const graph = buildAudioGraph(
      selectAudioSources(doc, (id) => ({ path: `${id}.wav`, hasAudio: true })),
      30,
    )!;
    expect(graph.inputs).toEqual(['ast_a.wav', 'ast_b.wav']);
    const [first, second, mix] = graph.filterComplex.split(';');
    expect(first).toMatch(/^\[1:a\]atrim=start=1\.000:end=4\.000,asetpts=PTS-STARTPTS,volume='.*':eval=frame,adelay=5000:all=1\[a0\]$/);
    expect(second).toMatch(/^\[2:a\]atrim=.*,volume=1\.0000,adelay=0:all=1\[a1\]$/);
    expect(mix).toBe('[a0][a1]amix=inputs=2:normalize=0:duration=longest[mixed]');
    expect(items).toHaveLength(2);
  });
});

describe('atempoChain', () => {
  it('chains instances so speeds outside 0.5-2 are not clamped', () => {
    const product = (speed: number) => atempoChain(speed).reduce((p, f) => p * Number(f.split('=')[1]), 1);
    for (const speed of [0.1, 0.25, 0.75, 1.5, 4, 16]) {
      expect(product(speed)).toBeCloseTo(speed, 3);
    }
    expect(atempoChain(1.5)).toHaveLength(1);
  });
});

describe('volumeExpression', () => {
  const clip = (extra: Record<string, unknown>) => docWith([{ type: 'audio', startFrame: 300, durationFrames: 90, assetId: 'ast_m', ...extra }]).items[0]!;

  it('is a constant when nothing varies', () => {
    expect(volumeExpression(clip({ volume: 0.5 }), 30)).toBe('volume=0.5000');
  });

  it('applies a lone fade-in (previously dropped) in item-local time', () => {
    const expr = volumeExpression(clip({ props: { fadeInFrames: 30, fadeOutFrames: 0 } }), 30);
    expect(expr).toContain('min(1,max(0,t/1.0000))');
    expect(expr).not.toContain('(3.0000-t)');
  });

  it('interpolates keyframes linearly instead of stepping to the next value', () => {
    const expr = volumeExpression(
      clip({ keyframes: { volume: [{ frame: 30, value: 1, easing: 'linear' }, { frame: 60, value: 0.2, easing: 'linear' }] } }),
      30,
    );
    // between 1.0s and 2.0s: 1 + (-0.8) * (t-1)/1
    expect(expr).toContain('1.0000+(-0.8000)*(t-1.0000)/1.0000');
  });
});
