import { pathToFileURL } from 'node:url';
import type { Item, TimelineDoc } from '@cutboard/schema';
import { itemEnd } from '@cutboard/editor-core';

/**
 * OpenTimelineIO interchange (addendum §5.3): export the timeline as an .otio JSON
 * document for round-tripping into Resolve/Premiere/FCP. Text/caption/motion-graphic
 * items are exported as gaps (NLEs can't represent them as clips), so timing survives.
 *
 * OTIO tracks are sequential, so items that overlap on one Cutboard track are spread over
 * extra tracks; stack order is bottom-first, i.e. the reverse of Cutboard's top-first tracks.
 */

const time = (rate: number, value: number) => ({ OTIO_SCHEMA: 'RationalTime.1', rate, value });
const range = (rate: number, start: number, duration: number) => ({
  OTIO_SCHEMA: 'TimeRange.1',
  start_time: time(rate, start),
  duration: time(rate, duration),
});

function isMedia(item: Item): boolean {
  return (item.type === 'video' || item.type === 'audio' || item.type === 'image') && Boolean(item.assetId);
}

/** Greedy lanes: each lane holds items that do not overlap, in start order. */
function lanesOf(items: Item[]): Item[][] {
  const lanes: Item[][] = [];
  for (const item of [...items].sort((a, b) => a.startFrame - b.startFrame)) {
    const lane = lanes.find((l) => itemEnd(l[l.length - 1]!) <= item.startFrame);
    if (lane) lane.push(item);
    else lanes.push([item]);
  }
  return lanes;
}

export function buildOtio(doc: TimelineDoc, mediaFor: (assetId: string) => string | null): Record<string, unknown> {
  const fps = doc.project.fps;

  const clipOf = (item: Item): Record<string, unknown> => {
    // source_range is the part of the media used (before any speed change)
    const sourceStart = item.sourceInFrame ?? 0;
    const sourceFrames = Math.max(1, Math.round(item.durationFrames * item.speed));
    const path = mediaFor(item.assetId!);
    return {
      OTIO_SCHEMA: 'Clip.1',
      name: item.labels?.name ?? item.type,
      source_range: range(fps, sourceStart, sourceFrames),
      media_reference: path
        ? { OTIO_SCHEMA: 'ExternalReference.1', target_url: pathToFileURL(path).href, available_range: range(fps, 0, sourceStart + sourceFrames) }
        : { OTIO_SCHEMA: 'MissingReference.1' },
      effects:
        item.speed !== 1
          ? [{ OTIO_SCHEMA: 'LinearTimeWarp.1', name: 'speed', effect_name: 'LinearTimeWarp', time_scalar: item.speed, metadata: {} }]
          : [],
      markers: [],
      metadata: {},
    };
  };

  const gap = (duration: number, name?: string): Record<string, unknown> => ({
    OTIO_SCHEMA: 'Gap.1',
    ...(name ? { name } : {}),
    source_range: range(fps, 0, duration),
    effects: [],
    markers: [],
    metadata: {},
  });

  const tracks: Record<string, unknown>[] = [];
  for (const track of doc.tracks) {
    const items = doc.items.filter((i) => i.trackId === track.id);
    if (!items.some(isMedia)) continue; // nothing an NLE could use (e.g. a captions-only track)
    lanesOf(items).forEach((lane, laneIndex) => {
      const children: Record<string, unknown>[] = [];
      let cursor = 0;
      for (const item of lane) {
        if (item.startFrame > cursor) children.push(gap(item.startFrame - cursor));
        children.push(isMedia(item) ? clipOf(item) : gap(item.durationFrames, `${item.type}: ${item.labels?.name ?? ''}`.trim()));
        cursor = itemEnd(item);
      }
      tracks.push({
        OTIO_SCHEMA: 'Track.1',
        name: laneIndex === 0 ? track.name : `${track.name} (${laneIndex + 1})`,
        kind: track.kind === 'audio' ? 'Audio' : 'Video',
        children,
        source_range: null,
        effects: [],
        markers: [],
        metadata: {},
      });
    });
  }

  return {
    OTIO_SCHEMA: 'Timeline.1',
    name: doc.project.name,
    metadata: { cutboard: { fps, projectId: doc.project.id } },
    global_start_time: time(fps, 0),
    tracks: {
      OTIO_SCHEMA: 'Stack.1',
      name: 'tracks',
      children: tracks.reverse(), // Cutboard lists the top track first, OTIO the bottom one
      source_range: null,
      effects: [],
      markers: [],
      metadata: {},
    },
  };
}
