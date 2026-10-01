import { TimelineDoc } from '@cutboard/schema';
import { itemEnd } from '@cutboard/editor-core';

/**
 * OpenTimelineIO interchange (addendum §5.3): export the timeline as an .otio JSON
 * document for round-tripping into Resolve/Premiere/FCP. Text/caption/motion-graphic
 * items are exported as GAP markers' neighbors' context only — NLEs can't represent
 * them as clips, so they are skipped (documented limitation).
 */
export function buildOtio(doc: TimelineDoc, mediaFor: (assetId: string) => string | null): Record<string, unknown> {
  const fps = doc.project.fps;
  const rate = { OTIO_SCHEMA: 'RationalTime.1', rate: fps };

  const tracks = doc.tracks
    .filter((t) => t.kind === 'video' || t.kind === 'audio' || t.kind === 'overlay' || t.kind === 'text')
    .map((track) => {
      const items = doc.items
        .filter((i) => i.trackId === track.id)
        .sort((a, b) => a.startFrame - b.startFrame);
      const children: Record<string, unknown>[] = [];
      let cursor = 0;
      for (const item of items) {
        if (item.startFrame > cursor) {
          children.push({
            OTIO_SCHEMA: 'Gap.1',
            source_range: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, duration: item.startFrame - cursor },
          });
        }
        const hasMedia = item.type === 'video' || item.type === 'audio' || item.type === 'image';
        if (hasMedia && item.assetId) {
          const target = mediaFor(item.assetId);
          children.push({
            OTIO_SCHEMA: 'Clip.1',
            name: item.labels?.name ?? item.type,
            source_range: {
              ...rate,
              start_time: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, value: item.sourceInFrame ?? 0 },
              duration: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, value: item.durationFrames },
            },
            media_reference: target
              ? { OTIO_SCHEMA: 'ExternalReference.1', target_url: target, available_range: { ...rate, start_time: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, value: 0 }, duration: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, value: itemEnd(item) - item.startFrame } } }
              : undefined,
          });
        } else {
          // non-representable item → Gap of the same length so timing survives
          children.push({
            OTIO_SCHEMA: 'Gap.1',
            name: `${item.type}: ${item.labels?.name ?? ''}`.trim(),
            source_range: { ...rate, duration: item.durationFrames },
          });
        }
        cursor = itemEnd(item);
      }
      return {
        OTIO_SCHEMA: 'Track.1',
        name: track.name,
        kind: track.kind === 'audio' ? 'Audio' : 'Video',
        children,
      };
    });

  return {
    OTIO_SCHEMA: 'Timeline.1',
    name: doc.project.name,
    metadata: { cutboard: { fps, projectId: doc.project.id } },
    global_start_time: { OTIO_SCHEMA: 'RationalTime.1', rate: fps, value: 0 },
    tracks: { OTIO_SCHEMA: 'Stack.1', name: 'tracks', children: tracks },
  };
}
