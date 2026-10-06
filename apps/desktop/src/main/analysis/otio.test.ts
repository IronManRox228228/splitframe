import { describe, expect, it } from 'vitest';
import { newId } from '@cutboard/schema';
import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import { buildOtio } from './otio.ts';

interface OtioNode {
  OTIO_SCHEMA: string;
  [key: string]: any; // eslint-disable-line @typescript-eslint/no-explicit-any
}

function sample() {
  const doc0 = createEmptyDoc({ id: newId('prj'), name: 'Cut', fps: 30 });
  const video = doc0.tracks.find((t) => t.kind === 'video')!;
  const audio = doc0.tracks.find((t) => t.kind === 'audio')!;
  const text = doc0.tracks.find((t) => t.kind === 'text')!;
  const make = (type: 'video' | 'audio', trackId: string, startFrame: number, durationFrames: number, extra: Record<string, unknown> = {}) =>
    createItem(type, { id: newId('itm'), trackId, startFrame, durationFrames, assetId: type === 'video' ? 'ast_v' : 'ast_a', sourceInFrame: 30, ...extra } as never);
  const items = [
    make('video', video.id, 15, 60),
    make('video', video.id, 75, 30, { speed: 2 }),
    make('video', video.id, 90, 30), // overlaps the previous clip
    make('audio', audio.id, 0, 120),
    createItem('text', { id: newId('itm'), trackId: text.id, startFrame: 0, durationFrames: 30, props: { text: 'hi', style: { fontFamily: 'Inter', fontSize: 64, color: '#fff' } } } as never),
  ];
  const doc = applyOps(doc0, items.map((item) => ({ type: 'item.add' as const, item }))).doc;
  return { doc, items };
}

const paths: Record<string, string> = { ast_v: '/media/My Clips/a.mp4', ast_a: '/media/music.wav' };
const build = () => buildOtio(sample().doc, (id) => paths[id] ?? null) as OtioNode;

describe('buildOtio', () => {
  it('uses OTIO TimeRange/RationalTime objects, not bare numbers', () => {
    const otio = build();
    expect(otio['OTIO_SCHEMA']).toBe('Timeline.1');
    const tracks = otio['tracks'].children as OtioNode[];
    const videoTrack = tracks.find((t) => t['kind'] === 'Video' && t['children'][0].source_range.duration.value === 15)!;
    const [gap, clip] = videoTrack['children'] as OtioNode[];
    expect(gap!['OTIO_SCHEMA']).toBe('Gap.1');
    expect(gap!['source_range']).toEqual({
      OTIO_SCHEMA: 'TimeRange.1',
      start_time: { OTIO_SCHEMA: 'RationalTime.1', rate: 30, value: 0 },
      duration: { OTIO_SCHEMA: 'RationalTime.1', rate: 30, value: 15 },
    });
    expect(clip!['source_range'].OTIO_SCHEMA).toBe('TimeRange.1');
    expect(clip!['source_range'].start_time.value).toBe(30);
    expect(clip!['source_range'].duration.value).toBe(60);
  });

  it('links original media as file URLs and flags unknown media as missing', () => {
    const doc = sample().doc;
    const withMedia = buildOtio(doc, (id) => paths[id] ?? null) as OtioNode;
    const clip = (withMedia['tracks'].children as OtioNode[]).find((t) => t['kind'] === 'Video')!['children'][1];
    expect(clip.media_reference.OTIO_SCHEMA).toBe('ExternalReference.1');
    expect(clip.media_reference.target_url).toMatch(/^file:\/\/\/.*My%20Clips\/a\.mp4$/);
    const without = buildOtio(doc, () => null) as OtioNode;
    const missing = (without['tracks'].children as OtioNode[]).find((t) => t['kind'] === 'Video')!['children'][1];
    expect(missing.media_reference.OTIO_SCHEMA).toBe('MissingReference.1');
  });

  it('keeps speed as a time warp and the used source length', () => {
    const tracks = build()['tracks'].children as OtioNode[];
    const clips = tracks.flatMap((t) => t['children'] as OtioNode[]).filter((c) => c['OTIO_SCHEMA'] === 'Clip.1');
    const fast = clips.find((c) => c['effects'].length > 0)!;
    expect(fast['effects'][0]).toMatchObject({ OTIO_SCHEMA: 'LinearTimeWarp.1', time_scalar: 2 });
    expect(fast['source_range'].duration.value).toBe(60); // 30 timeline frames at 2x use 60 source frames
  });

  it('spreads overlapping clips over an extra track instead of overlapping in one track', () => {
    const tracks = build()['tracks'].children as OtioNode[];
    const video = tracks.filter((t) => t['kind'] === 'Video');
    expect(video).toHaveLength(2);
    // the overlapping clip starts at frame 90: a 90-frame gap, then 30 frames of clip
    const extra = video.find((t) => (t['children'] as OtioNode[]).length === 2)!;
    const [lead, clip] = extra['children'] as OtioNode[];
    expect(lead!['source_range'].duration.value).toBe(90);
    expect(clip!['source_range'].duration.value).toBe(30);
  });

  it('lists the bottom track first and skips tracks without media', () => {
    const tracks = build()['tracks'].children as OtioNode[];
    expect(tracks.map((t) => t['kind'])).toEqual(['Audio', 'Video', 'Video']); // audio is the lowest track; the text-only track is gone
  });
});
