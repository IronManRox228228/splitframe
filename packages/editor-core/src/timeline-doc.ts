import {
  DEFAULT_TRANSFORM,
  Item,
  ItemOfType,
  ItemType,
  itemPropsSchemas,
  itemSchema,
  timelineDocSchema,
  TimelineDoc,
  Track,
  TrackKind,
  trackSchema,
  projectSchema,
} from '@cutboard/schema';

export class OpError extends Error {
  /** Actionable hint for agents/UI on how to recover. */
  readonly hint: string;
  constructor(message: string, hint = 'Check the referenced ids exist and the values are valid.') {
    super(message);
    this.name = 'OpError';
    this.hint = hint;
  }
}

export const TRACK_KIND_ALLOWED: Record<TrackKind, readonly ItemType[]> = {
  video: ['video', 'image', 'shape', 'motionGraphic'],
  overlay: ['video', 'image', 'shape', 'motionGraphic'],
  text: ['text', 'caption', 'shape', 'motionGraphic'],
  audio: ['audio'],
};

export const MEDIA_TYPES: readonly ItemType[] = ['video', 'audio'];

export function trackAllowsItem(trackKind: TrackKind, itemType: ItemType): boolean {
  return TRACK_KIND_ALLOWED[trackKind].includes(itemType);
}

export function getTrack(doc: TimelineDoc, trackId: string): Track | undefined {
  return doc.tracks.find((t) => t.id === trackId);
}

export function getItem(doc: TimelineDoc, itemId: string): Item | undefined {
  return doc.items.find((i) => i.id === itemId);
}

export function requireItem(doc: TimelineDoc, itemId: string): Item {
  const item = getItem(doc, itemId);
  if (!item) {
    throw new OpError(`Item ${itemId} not found.`, 'Call getTimeline to list current item ids.');
  }
  return item;
}

export function requireTrack(doc: TimelineDoc, trackId: string): Track {
  const track = getTrack(doc, trackId);
  if (!track) {
    throw new OpError(`Track ${trackId} not found.`, 'Call getTimeline to list current track ids.');
  }
  return track;
}

export function itemEnd(item: Item): number {
  return item.startFrame + item.durationFrames;
}

export function itemsOnTrack(doc: TimelineDoc, trackId: string): Item[] {
  return doc.items
    .filter((i) => i.trackId === trackId)
    .sort((a, b) => a.startFrame - b.startFrame || a.id.localeCompare(b.id));
}

export function docDurationFrames(doc: TimelineDoc): number {
  return doc.items.reduce((max, i) => Math.max(max, itemEnd(i)), 0);
}

/** Derived source out point (sourceIn + durationFrames * speed); 0 for non-media items. */
export function sourceOutFrame(item: Item): number {
  if (!MEDIA_TYPES.includes(item.type) || item.sourceInFrame === undefined) return 0;
  return item.sourceInFrame + Math.round(item.durationFrames * item.speed);
}

/**
 * Map a timeline frame (absolute) to a source-local frame. Uses the piecewise-linear
 * time remap when present, otherwise constant speed.
 */
export function sourceFrameAt(item: Item, timelineFrame: number): number {
  const local = timelineFrame - item.startFrame;
  const base = item.sourceInFrame ?? 0;
  if (item.timeRemap && item.timeRemap.length > 0) {
    const points = [...item.timeRemap].sort((a, b) => a.frame - b.frame);
    if (local <= points[0]!.frame) {
      // extrapolate linearly from the first segment (or flat if single point)
      if (points.length === 1) return Math.max(0, Math.round(base + local));
      const p0 = points[0]!;
      const p1 = points[1]!;
      const slope = (p1.sourceFrame - p0.sourceFrame) / Math.max(1, p1.frame - p0.frame);
      return Math.max(0, Math.round(p0.sourceFrame + (local - p0.frame) * slope));
    }
    const last = points[points.length - 1]!;
    if (local >= last.frame) {
      if (points.length === 1) return Math.max(0, Math.round(last.sourceFrame + (local - last.frame)));
      const p0 = points[points.length - 2]!;
      const slope = (last.sourceFrame - p0.sourceFrame) / Math.max(1, last.frame - p0.frame);
      return Math.max(0, Math.round(last.sourceFrame + (local - last.frame) * slope));
    }
    for (let i = 0; i < points.length - 1; i++) {
      const a = points[i]!;
      const b = points[i + 1]!;
      if (local >= a.frame && local <= b.frame) {
        const t = (local - a.frame) / Math.max(1, b.frame - a.frame);
        return Math.max(0, Math.round(a.sourceFrame + t * (b.sourceFrame - a.sourceFrame)));
      }
    }
  }
  return Math.max(0, Math.round(base + local * item.speed));
}

export function isAudioBearing(item: Item): boolean {
  return item.type === 'audio' || item.type === 'video';
}

export function isVisual(item: Item): boolean {
  return item.type !== 'audio';
}

export function clone<T>(value: T): T {
  return structuredClone(value);
}

export function createItem<K extends ItemType>(
  type: K,
  input: Partial<ItemOfType<K>> & Pick<ItemOfType<K>, 'id' | 'trackId' | 'startFrame' | 'durationFrames'>,
): ItemOfType<K> {
  const base = {
    id: input.id,
    trackId: input.trackId,
    type,
    startFrame: input.startFrame,
    durationFrames: input.durationFrames,
    assetId: input.assetId,
    sourceInFrame: input.sourceInFrame,
    speed: input.speed ?? 1,
    timeRemap: input.timeRemap ?? [],
    transform: { ...DEFAULT_TRANSFORM, ...input.transform },
    volume: input.volume ?? 1,
    muted: input.muted ?? false,
    effects: input.effects ?? [],
    masks: input.masks ?? [],
    keyframes: input.keyframes ?? {},
    labels: input.labels ?? {},
    props: (input.props ?? {}) as ItemOfType<K>['props'],
  };
  const parsed = itemSchema.parse(base);
  if (MEDIA_TYPES.includes(type) && !parsed.assetId) {
    throw new OpError(`${type} items require an assetId.`, 'Import the asset first and pass its assetId.');
  }
  return parsed as ItemOfType<K>;
}

export function createEmptyDoc(input: {
  id: string;
  name: string;
  fps?: number;
  width?: number;
  height?: number;
}): TimelineDoc {
  const now = new Date().toISOString();
  const mkTrack = (id: string, kind: TrackKind, name: string): Track => trackSchema.parse({ id, kind, name });
  return {
    project: projectSchema.parse({
      id: input.id,
      name: input.name,
      fps: input.fps ?? 30,
      width: input.width ?? 1920,
      height: input.height ?? 1080,
      createdAt: now,
      updatedAt: now,
    }),
    tracks: [
      mkTrack(`${input.id}_text`, 'text', 'Text & Graphics'),
      mkTrack(`${input.id}_main`, 'video', 'Main Video'),
      mkTrack(`${input.id}_audio`, 'audio', 'Audio 1'),
    ],
    items: [],
    markers: [],
  };
}

/** Validate a whole doc (used when loading from SQLite). */
export function parseDoc(raw: unknown): TimelineDoc {
  return timelineDocSchema.parse(raw);
}
