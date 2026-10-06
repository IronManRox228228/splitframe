import {
  Item,
  ItemPatch,
  Op,
  OpEntry,
  opSchema,
  TimelineDoc,
  Track,
  TransformPatch,
  maskSchema,
  itemPropsSchemas,
} from '@cutboard/schema';
import {
  OpError,
  clone,
  isAudioBearing,
  itemEnd,
  MEDIA_TYPES,
  requireItem,
  requireTrack,
  trackAllowsItem,
} from './timeline-doc.ts';

export interface ApplyResult {
  doc: TimelineDoc;
  /** Ops that undo this op, computed against the pre-op doc. Apply in order. */
  inverse: Op[];
}

export interface ApplyOptions {
  /**
   * Reject edits to items on locked tracks (default true). Undo/redo replay recorded
   * inverses and pass false, so a track locked after an edit can still be undone.
   */
  enforceLocks?: boolean;
}

/**
 * Apply one op to a doc and return a new doc plus its inverse.
 * Pure: the input doc is never mutated. Atomic: on error the input doc is untouched.
 */
export function applyOp(doc: TimelineDoc, op: Op, opts: ApplyOptions = {}): ApplyResult {
  const parsed = opSchema.parse(op); // throws ZodError on malformed ops
  if (opts.enforceLocks !== false) assertTracksUnlocked(doc, parsed);
  const next = clone(doc);
  switch (parsed.type) {
    case 'batch':
      return applyBatch(doc, parsed.ops, opts);
    case 'project.rename':
      return simple(next, doc, (d) => {
        d.project.name = parsed.name;
      }, [{ type: 'project.rename', name: doc.project.name }]);
    case 'project.setCanvas':
      return simple(next, doc, (d) => {
        d.project.width = parsed.width;
        d.project.height = parsed.height;
      }, [{ type: 'project.setCanvas', width: doc.project.width, height: doc.project.height }]);
    case 'project.setStyleConfig':
      return simple(next, doc, (d) => {
        d.project.styleConfig = parsed.styleConfig;
      }, [{ type: 'project.setStyleConfig', styleConfig: clone(doc.project.styleConfig) }]);
    case 'project.setReference': {
      const before = doc.project.referenceAssetId ?? null;
      if (parsed.assetId !== null && !doc.items.some((i) => i.assetId === parsed.assetId)) {
        // references are metadata, not timeline items — validate against assets in the tool layer
      }
      return simple(next, doc, (d) => {
        d.project.referenceAssetId = parsed.assetId ?? undefined;
      }, [{ type: 'project.setReference', assetId: before }]);
    }
    case 'track.add':
      return applyTrackAdd(next, doc, parsed);
    case 'track.remove':
      return applyTrackRemove(next, doc, parsed);
    case 'track.update':
      return applyTrackUpdate(next, doc, parsed);
    case 'track.reorder':
      return applyTrackReorder(next, doc, parsed);
    case 'item.add':
      return applyItemAdd(next, doc, parsed);
    case 'item.remove':
      return applyItemRemove(next, doc, parsed);
    case 'item.update':
      return applyItemUpdate(next, doc, parsed);
    case 'item.move':
      return applyItemMove(next, doc, parsed);
    case 'item.trim':
      return applyItemTrim(next, doc, parsed);
    case 'item.split':
      return applyItemSplit(next, doc, parsed);
    case 'item.clone':
      return applyItemClone(next, doc, parsed);
    case 'item.slip':
      return applyItemSlip(next, doc, parsed);
    case 'item.setSpeed':
      return applyItemSetSpeed(next, doc, parsed);
    case 'item.setTimeRemap':
      return applyItemSetTimeRemap(next, doc, parsed);
    case 'item.setKeyframes':
      return applyItemSetKeyframes(next, doc, parsed);
    case 'effect.add':
    case 'effect.remove':
    case 'effect.update':
      return applyEffectOp(next, doc, parsed);
    case 'mask.add':
    case 'mask.remove':
      return applyMaskOp(next, doc, parsed);
    case 'marker.add':
      return simple(next, doc, (d) => {
        d.markers.push(clone(parsed.marker));
      }, [{ type: 'marker.remove', markerId: parsed.marker.id }]);
    case 'marker.remove': {
      const idx = doc.markers.findIndex((m) => m.id === parsed.markerId);
      if (idx === -1) throw new OpError(`Marker ${parsed.markerId} not found.`);
      const marker = doc.markers[idx]!;
      return simple(next, doc, (d) => {
        d.markers.splice(d.markers.findIndex((m) => m.id === parsed.markerId), 1);
      }, [{ type: 'marker.add', marker: clone(marker) }]);
    }
    case 'marker.update': {
      const marker = doc.markers.find((m) => m.id === parsed.markerId);
      if (!marker) throw new OpError(`Marker ${parsed.markerId} not found.`);
      const before = { frame: marker.frame, label: marker.label, color: marker.color };
      return simple(next, doc, (d) => {
        const m = d.markers.find((x) => x.id === parsed.markerId)!;
        if (parsed.patch.frame !== undefined) m.frame = parsed.patch.frame;
        if (parsed.patch.label !== undefined) m.label = parsed.patch.label;
        if (parsed.patch.color !== undefined) m.color = parsed.patch.color;
      }, [{ type: 'marker.update', markerId: parsed.markerId, patch: before }]);
    }
    default:
      throw new OpError(`Unknown op type.`, 'The op was validated, so this should be unreachable.');
  }
}

/** Apply a list of ops; inverses are flattened in reverse so they undo the whole list. */
export function applyOps(doc: TimelineDoc, ops: Op[], opts: ApplyOptions = {}): ApplyResult {
  let current = doc;
  const inverses: Op[] = [];
  for (const op of ops) {
    const r = applyOp(current, op, opts);
    current = r.doc;
    inverses.unshift(...r.inverse);
  }
  return { doc: current, inverse: inverses };
}

function simple(
  next: TimelineDoc,
  _before: TimelineDoc,
  mutate: (d: TimelineDoc) => void,
  inverse: Op[],
): ApplyResult {
  mutate(next);
  return { doc: next, inverse };
}

function applyBatch(doc: TimelineDoc, ops: Op[], opts: ApplyOptions): ApplyResult {
  for (const op of ops) {
    if (op.type === 'batch') {
      throw new OpError('Nested batch ops are not allowed.', 'Flatten the op list.');
    }
  }
  return applyOps(doc, ops, opts);
}

/** Tracks an op would modify: its items' tracks and any destination track. */
function assertTracksUnlocked(doc: TimelineDoc, op: Op): void {
  const trackIds = new Set<string>();
  const ofItem = (itemId: string) => {
    const item = doc.items.find((i) => i.id === itemId);
    if (item) trackIds.add(item.trackId);
  };
  switch (op.type) {
    case 'item.add':
      trackIds.add(op.item.trackId);
      break;
    case 'item.remove':
      op.itemIds.forEach(ofItem);
      break;
    case 'item.update':
      ofItem(op.itemId);
      if (op.patch.trackId) trackIds.add(op.patch.trackId);
      break;
    case 'item.move':
      ofItem(op.itemId);
      if (op.trackId) trackIds.add(op.trackId);
      break;
    case 'item.clone': {
      // reading from a locked track is fine; the copy lands on the destination
      const src = doc.items.find((i) => i.id === op.itemId);
      trackIds.add(op.trackId ?? src?.trackId ?? '');
      break;
    }
    case 'track.remove':
      trackIds.add(op.trackId);
      break;
    case 'item.trim':
    case 'item.split':
    case 'item.slip':
    case 'item.setSpeed':
    case 'item.setTimeRemap':
    case 'item.setKeyframes':
    case 'effect.add':
    case 'effect.remove':
    case 'effect.update':
    case 'mask.add':
    case 'mask.remove':
      ofItem(op.itemId);
      break;
    default:
      return;
  }
  for (const id of trackIds) {
    const track = doc.tracks.find((t) => t.id === id);
    if (track?.locked) throw new OpError(`Track "${track.name}" is locked.`, 'Unlock the track first.');
  }
}

// ---------- tracks ----------

function applyTrackAdd(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'track.add' }>,
): ApplyResult {
  if (doc.tracks.some((t) => t.id === op.trackId)) {
    throw new OpError(`Track ${op.trackId} already exists.`);
  }
  const track: Track = {
    id: op.trackId,
    kind: op.kind,
    name: op.name,
    locked: op.locked,
    muted: op.muted,
    hidden: op.hidden,
  };
  const index = op.index ?? doc.tracks.length;
  next.tracks.splice(index, 0, track);
  return { doc: next, inverse: [{ type: 'track.remove', trackId: op.trackId }] };
}

function applyTrackRemove(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'track.remove' }>,
): ApplyResult {
  const index = doc.tracks.findIndex((t) => t.id === op.trackId);
  if (index === -1) throw new OpError(`Track ${op.trackId} not found.`);
  const track = doc.tracks[index]!;
  const removedItems = doc.items.filter((i) => i.trackId === op.trackId);
  next.tracks.splice(index, 1);
  next.items = next.items.filter((i) => i.trackId !== op.trackId);
  // restore the track at its old index, then the items
  const inverse: Op[] = [
    {
      type: 'track.add',
      trackId: track.id,
      kind: track.kind,
      name: track.name,
      locked: track.locked,
      muted: track.muted,
      hidden: track.hidden,
      index,
    },
    ...removedItems.map((i) => ({ type: 'item.add', item: clone(i) }) as Op),
  ];
  return { doc: next, inverse };
}

function applyTrackUpdate(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'track.update' }>,
): ApplyResult {
  const track = doc.tracks.find((t) => t.id === op.trackId);
  if (!track) throw new OpError(`Track ${op.trackId} not found.`);
  const before = {
    name: track.name,
    locked: track.locked,
    muted: track.muted,
    hidden: track.hidden,
  };
  const inverse: Op[] = [
    {
      type: 'track.update',
      trackId: op.trackId,
      patch: {
        name: op.patch.name !== undefined ? before.name : undefined,
        locked: op.patch.locked !== undefined ? before.locked : undefined,
        muted: op.patch.muted !== undefined ? before.muted : undefined,
        hidden: op.patch.hidden !== undefined ? before.hidden : undefined,
      },
    },
  ];
  return simple(next, doc, (d) => {
    const t = d.tracks.find((x) => x.id === op.trackId)!;
    if (op.patch.name !== undefined) t.name = op.patch.name;
    if (op.patch.locked !== undefined) t.locked = op.patch.locked;
    if (op.patch.muted !== undefined) t.muted = op.patch.muted;
    if (op.patch.hidden !== undefined) t.hidden = op.patch.hidden;
  }, inverse);
}

function applyTrackReorder(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'track.reorder' }>,
): ApplyResult {
  const current = doc.tracks.map((t) => t.id).sort().join(',');
  const proposed = [...op.trackIds].sort().join(',');
  if (current !== proposed) {
    throw new OpError(
      'track.reorder must contain exactly the existing track ids.',
      'Call getTimeline to read the current order first.',
    );
  }
  const byId = new Map<string, Track>(doc.tracks.map((t) => [t.id, t] as const));
  next.tracks = op.trackIds.map((id) => byId.get(id)!);
  return {
    doc: next,
    inverse: [{ type: 'track.reorder', trackIds: doc.tracks.map((t) => t.id) }],
  };
}

// ---------- items ----------

function applyItemAdd(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.add' }>,
): ApplyResult {
  const rawItem = op.item;
  const track = requireTrack(doc, rawItem.trackId);
  if (doc.items.some((i) => i.id === rawItem.id)) {
    throw new OpError(`Item ${rawItem.id} already exists.`);
  }
  if (!trackAllowsItem(track.kind, rawItem.type)) {
    throw new OpError(
      `A ${rawItem.type} item cannot live on the "${track.kind}" track "${track.name}".`,
      `Allowed item types on a ${track.kind} track: see the track-kind rules.`,
    );
  }
  // normalize: type-specific props may be omitted by tools; fill from the schema defaults
  const item = {
    ...rawItem,
    props: rawItem.props ?? itemPropsSchemas[rawItem.type].parse({}),
  } as Item;
  next.items.push(clone(item));
  return { doc: next, inverse: [{ type: 'item.remove', itemIds: [item.id], ripple: false }] };
}

function applyItemRemove(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.remove' }>,
): ApplyResult {
  const missing = op.itemIds.filter((id) => !doc.items.some((i) => i.id === id));
  if (missing.length > 0) {
    throw new OpError(`Items not found: ${missing.join(', ')}.`);
  }
  const removed = doc.items.filter((i) => op.itemIds.includes(i.id));

  // ripple: per track, close the gap left by the removed items
  const shifts = new Map<string, number>(); // itemId -> delta
  if (op.ripple) {
    const byTrack = new Map<string, Item[]>();
    for (const item of removed) {
      const list = byTrack.get(item.trackId) ?? [];
      list.push(item);
      byTrack.set(item.trackId, list);
    }
    for (const [trackId, removedOnTrack] of byTrack) {
      const gap = removedOnTrack.reduce((sum, i) => sum + i.durationFrames, 0);
      const minStart = Math.min(...removedOnTrack.map((i) => i.startFrame));
      const maxEnd = Math.max(...removedOnTrack.map(itemEnd));
      for (const item of doc.items) {
        if (item.trackId !== trackId || op.itemIds.includes(item.id)) continue;
        if (item.startFrame >= maxEnd) shifts.set(item.id, -gap);
        else if (item.startFrame >= minStart) {
          // sits inside the removed span: pull it back to the region start
          shifts.set(item.id, minStart - item.startFrame);
        }
      }
    }
  }

  next.items = next.items.filter((i) => !op.itemIds.includes(i.id));
  for (const item of next.items) {
    const delta = shifts.get(item.id);
    if (delta !== undefined) item.startFrame = Math.max(0, item.startFrame + delta);
  }

  const inverse: Op[] = removed.map((i) => ({ type: 'item.add', item: clone(i) }) as Op);
  for (const [itemId, delta] of shifts) {
    const before = doc.items.find((i) => i.id === itemId)!;
    inverse.push({ type: 'item.update', itemId, patch: { startFrame: before.startFrame } });
  }
  return { doc: next, inverse };
}

function applyItemUpdate(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.update' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  const patch = op.patch;

  if (patch.trackId !== undefined && patch.trackId !== item.trackId) {
    const track = requireTrack(doc, patch.trackId);
    if (!trackAllowsItem(track.kind, item.type)) {
      throw new OpError(`A ${item.type} item cannot move to the "${track.kind}" track "${track.name}".`);
    }
  }
  if (patch.props !== undefined) {
    // whole-replace; shape-check against the item's own type
    itemPropsSchemas[item.type].parse(patch.props);
  }

  const inversePatch: ItemPatch = {};
  if (patch.trackId !== undefined) inversePatch.trackId = item.trackId;
  if (patch.startFrame !== undefined) inversePatch.startFrame = item.startFrame;
  if (patch.durationFrames !== undefined) inversePatch.durationFrames = item.durationFrames;
  if (patch.sourceInFrame !== undefined) inversePatch.sourceInFrame = item.sourceInFrame;
  if (patch.speed !== undefined) inversePatch.speed = item.speed;
  if (patch.timeRemap !== undefined) inversePatch.timeRemap = clone(item.timeRemap);
  if (patch.transform !== undefined) {
    const t: TransformPatch = {};
    for (const key of Object.keys(patch.transform) as (keyof typeof patch.transform)[]) {
      (t as Record<string, unknown>)[key] = item.transform[key];
    }
    inversePatch.transform = t;
  }
  if (patch.volume !== undefined) inversePatch.volume = item.volume;
  if (patch.muted !== undefined) inversePatch.muted = item.muted;
  if (patch.effects !== undefined) inversePatch.effects = clone(item.effects);
  if (patch.masks !== undefined) inversePatch.masks = clone(item.masks);
  if (patch.keyframes !== undefined) {
    const kf: Record<string, Item['keyframes'][string]> = {};
    for (const prop of Object.keys(patch.keyframes)) {
      kf[prop] = clone(item.keyframes[prop] ?? []);
    }
    inversePatch.keyframes = kf;
  }
  if (patch.labels !== undefined) {
    // labels merge, so the inverse names every touched key (undefined = remove it again)
    const l: Record<string, string | undefined> = {};
    for (const key of Object.keys(patch.labels)) l[key] = (item.labels as Record<string, string | undefined>)[key];
    inversePatch.labels = l;
  }
  if (patch.props !== undefined) inversePatch.props = clone(item.props);

  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    if (patch.trackId !== undefined) target.trackId = patch.trackId;
    if (patch.startFrame !== undefined) target.startFrame = Math.max(0, patch.startFrame);
    if (patch.durationFrames !== undefined) target.durationFrames = Math.max(1, patch.durationFrames);
    if (patch.sourceInFrame !== undefined) target.sourceInFrame = Math.max(0, patch.sourceInFrame);
    if (patch.speed !== undefined) target.speed = patch.speed;
    if (patch.timeRemap !== undefined) target.timeRemap = clone(patch.timeRemap);
    if (patch.transform !== undefined) target.transform = { ...target.transform, ...patch.transform };
    if (patch.volume !== undefined) target.volume = patch.volume;
    if (patch.muted !== undefined) target.muted = patch.muted;
    if (patch.effects !== undefined) target.effects = clone(patch.effects);
    if (patch.masks !== undefined) target.masks = clone(patch.masks);
    if (patch.keyframes !== undefined) {
      for (const [prop, kfs] of Object.entries(patch.keyframes)) {
        if (kfs.length === 0) delete target.keyframes[prop];
        else target.keyframes[prop] = clone(kfs);
      }
    }
    if (patch.labels !== undefined) {
      const labels: Record<string, string | undefined> = { ...target.labels };
      for (const [key, value] of Object.entries(patch.labels)) {
        if (value === undefined) delete labels[key];
        else labels[key] = value;
      }
      target.labels = labels;
    }
    if (patch.props !== undefined) target.props = clone(patch.props) as typeof target.props;
  }, [{ type: 'item.update', itemId: op.itemId, patch: inversePatch }]);
}

function applyItemMove(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.move' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  const oldTrackId = item.trackId;
  const oldStart = item.startFrame;
  if (op.trackId !== undefined && op.trackId !== oldTrackId) {
    const track = requireTrack(doc, op.trackId);
    if (!trackAllowsItem(track.kind, item.type)) {
      throw new OpError(`A ${item.type} item cannot move to the "${track.kind}" track "${track.name}".`);
    }
  }
  if (op.trackId === undefined && op.startFrame === undefined) {
    throw new OpError('item.move needs a trackId or startFrame.');
  }
  const inverse: Op[] = [];
  if (op.trackId !== undefined) inverse.push({ type: 'item.move', itemId: item.id, trackId: oldTrackId });
  if (op.startFrame !== undefined) {
    inverse.push({ type: 'item.move', itemId: item.id, startFrame: oldStart });
  }
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    if (op.trackId !== undefined) target.trackId = op.trackId;
    if (op.startFrame !== undefined) target.startFrame = Math.max(0, op.startFrame);
  }, inverse);
}

interface TrimSpec {
  itemId: string;
  edge: 'in' | 'out';
  frame: number;
  ripple: boolean;
}

/**
 * Shared trim logic. Plain trim keeps the opposite edge fixed; ripple trim anchors the
 * item's start (for `in`) and slides downstream items by the delta so the timeline
 * length changes by exactly the trim amount.
 */
function computeTrim(doc: TimelineDoc, op: TrimSpec) {
  const item = requireItem(doc, op.itemId);
  const track = requireTrack(doc, item.trackId);
  if (track.locked) throw new OpError(`Track "${track.name}" is locked.`, 'Unlock the track first.');
  const oldStart = item.startFrame;
  const oldEnd = itemEnd(item);
  const isMedia = MEDIA_TYPES.includes(item.type);
  const oldSourceIn = item.sourceInFrame ?? 0;

  let newStart = oldStart;
  let newDuration = item.durationFrames;
  let newSourceIn = oldSourceIn;
  let downstreamDelta = 0;

  if (op.edge === 'in') {
    const delta = op.frame - oldStart; // >0: shorten head, <0: extend head
    // Furthest the head may extend: keep sourceIn >= 0 for media and, when the start moves
    // (plain trim), keep the start >= 0. Furthest it may shorten: leave one frame.
    const sourceLimit = isMedia && item.speed > 0 ? Math.ceil(-oldSourceIn / item.speed) : -Infinity;
    const minDelta = Math.max(sourceLimit, op.ripple ? -Infinity : -oldStart);
    const clampedDelta = Math.min(item.durationFrames - 1, Math.max(delta, minDelta));
    if (isMedia) newSourceIn = oldSourceIn + Math.round(clampedDelta * item.speed);
    if (op.ripple) {
      newStart = oldStart;
      newDuration = oldEnd - (oldStart + clampedDelta);
      downstreamDelta = -clampedDelta;
    } else {
      newStart = oldStart + clampedDelta;
      newDuration = oldEnd - newStart;
    }
  } else {
    const newEnd = Math.max(op.frame, oldStart + 1);
    newDuration = newEnd - oldStart;
    if (op.ripple) downstreamDelta = newEnd - oldEnd;
  }

  if (newStart === oldStart && newDuration === item.durationFrames && downstreamDelta === 0) {
    throw new OpError('Trim would not change the item.', 'Choose a frame outside the current edges.');
  }

  return { item, oldStart, oldEnd, oldSourceIn, newStart, newDuration, newSourceIn, downstreamDelta, isMedia };
}

function applyItemTrim(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.trim' }>,
): ApplyResult {
  const t = computeTrim(doc, op);

  next.items = next.items.map((i) => {
    if (i.id === t.item.id) {
      return { ...i, startFrame: t.newStart, durationFrames: t.newDuration, sourceInFrame: t.isMedia ? t.newSourceIn : i.sourceInFrame };
    }
    if (t.downstreamDelta !== 0 && i.trackId === t.item.trackId && i.startFrame >= t.oldEnd) {
      return { ...i, startFrame: Math.max(0, i.startFrame + t.downstreamDelta) };
    }
    return i;
  });

  // Restore the old geometry directly: re-running a trim would re-derive sourceIn from a
  // rounded value and could land a frame off at fractional speeds.
  const restore: ItemPatch = { startFrame: t.oldStart, durationFrames: t.item.durationFrames };
  if (t.isMedia && t.newSourceIn !== t.oldSourceIn) restore.sourceInFrame = t.oldSourceIn;
  const inverse: Op[] = [{ type: 'item.update', itemId: op.itemId, patch: restore }];
  if (t.downstreamDelta !== 0) {
    for (const i of doc.items) {
      if (i.trackId === t.item.trackId && i.id !== op.itemId && i.startFrame >= t.oldEnd) {
        inverse.push({ type: 'item.update', itemId: i.id, patch: { startFrame: i.startFrame } });
      }
    }
  }
  return { doc: next, inverse };
}

function applyItemSplit(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.split' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (op.atFrame <= item.startFrame || op.atFrame >= itemEnd(item)) {
    throw new OpError(
      `Split frame ${op.atFrame} is outside item ${item.id} (${item.startFrame}–${itemEnd(item)}).`,
      'Pick a frame strictly inside the item.',
    );
  }
  const splitLocal = op.atFrame - item.startFrame;
  const a: Item = { ...clone(item), durationFrames: splitLocal };
  const b: Item = {
    ...clone(item),
    id: op.newItemId,
    startFrame: op.atFrame,
    durationFrames: item.durationFrames - splitLocal,
    sourceInFrame: MEDIA_TYPES.includes(item.type)
      ? (item.sourceInFrame ?? 0) + Math.round(splitLocal * item.speed)
      : item.sourceInFrame,
    timeRemap: item.timeRemap
      .filter((p) => p.frame >= splitLocal)
      .map((p) => ({ ...p, frame: p.frame - splitLocal })),
  };
  a.timeRemap = item.timeRemap.filter((p) => p.frame < splitLocal);
  // split keyframe lists at the cut
  a.keyframes = {};
  b.keyframes = {};
  for (const [prop, kfs] of Object.entries(item.keyframes)) {
    a.keyframes[prop] = kfs.filter((k) => k.frame < splitLocal);
    b.keyframes[prop] = kfs
      .filter((k) => k.frame >= splitLocal)
      .map((k) => ({ ...k, frame: k.frame - splitLocal }));
  }

  const inverse: Op[] = [
    { type: 'item.remove', itemIds: [op.newItemId], ripple: false },
    { type: 'item.update', itemId: item.id, patch: { durationFrames: item.durationFrames, timeRemap: clone(item.timeRemap), keyframes: clone(item.keyframes) } },
  ];
  return simple(next, doc, (d) => {
    const idx = d.items.findIndex((i) => i.id === item.id)!;
    d.items[idx] = a;
    d.items.push(b);
  }, inverse);
}

function applyItemClone(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.clone' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (doc.items.some((i) => i.id === op.newItemId)) {
    throw new OpError(`Item ${op.newItemId} already exists.`);
  }
  if (op.trackId !== undefined && op.trackId !== item.trackId) {
    const track = requireTrack(doc, op.trackId);
    if (!trackAllowsItem(track.kind, item.type)) {
      throw new OpError(`A ${item.type} item cannot live on the "${track.kind}" track "${track.name}".`);
    }
  }
  const copy: Item = {
    ...clone(item),
    id: op.newItemId,
    trackId: op.trackId ?? item.trackId,
    startFrame: op.startFrame ?? itemEnd(item),
  };
  return simple(next, doc, (d) => {
    d.items.push(copy);
  }, [{ type: 'item.remove', itemIds: [op.newItemId], ripple: false }]);
}

function applyItemSlip(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.slip' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (!isAudioBearing(item)) {
    throw new OpError(`Only media items can slip (item ${item.id} is ${item.type}).`);
  }
  const newSourceIn = Math.max(0, op.sourceInFrame);
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    target.sourceInFrame = newSourceIn;
  }, [{ type: 'item.slip', itemId: op.itemId, sourceInFrame: item.sourceInFrame ?? 0 }]);
}

function applyItemSetSpeed(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.setSpeed' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (!MEDIA_TYPES.includes(item.type)) {
    throw new OpError(`Only media items can be retimed (item ${item.id} is ${item.type}).`);
  }
  const hadRemap = item.timeRemap.length > 0;
  const inverse: Op[] = [{ type: 'item.setSpeed', itemId: item.id, speed: item.speed }];
  if (hadRemap) inverse.push({ type: 'item.setTimeRemap', itemId: item.id, points: clone(item.timeRemap) });
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    target.speed = op.speed;
    if (hadRemap) target.timeRemap = [];
  }, inverse);
}

function applyItemSetTimeRemap(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.setTimeRemap' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (!MEDIA_TYPES.includes(item.type)) {
    throw new OpError(`Time remap applies to media items only (item ${item.id} is ${item.type}).`);
  }
  const points = [...op.points].sort((a, b) => a.frame - b.frame);
  for (const p of points) {
    if (p.frame < 0 || p.frame > item.durationFrames) {
      throw new OpError(
        `Time remap point at frame ${p.frame} is outside the item (0–${item.durationFrames}).`,
        'Use item-local frames.',
      );
    }
  }
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    target.timeRemap = points;
  }, [{ type: 'item.setTimeRemap', itemId: op.itemId, points: clone(item.timeRemap) }]);
}

function applyItemSetKeyframes(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'item.setKeyframes' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  const kfs = [...op.keyframes].sort((a, b) => a.frame - b.frame);
  const old = clone(item.keyframes[op.property] ?? []);
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    if (kfs.length === 0) delete target.keyframes[op.property];
    else target.keyframes[op.property] = kfs;
  }, [{ type: 'item.setKeyframes', itemId: op.itemId, property: op.property, keyframes: old }]);
}

// ---------- effects & masks ----------

function applyEffectOp(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'effect.add' | 'effect.remove' | 'effect.update' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (op.type === 'effect.add') {
    if (item.effects.some((e) => e.id === op.effect.id)) {
      throw new OpError(`Effect ${op.effect.id} already exists on item ${item.id}.`);
    }
    return simple(next, doc, (d) => {
      d.items.find((i) => i.id === op.itemId)!.effects.push(clone(op.effect));
    }, [{ type: 'effect.remove', itemId: op.itemId, effectId: op.effect.id }]);
  }
  const effect = item.effects.find((e) => e.id === op.effectId);
  if (!effect) throw new OpError(`Effect ${op.effectId} not found on item ${item.id}.`);
  if (op.type === 'effect.remove') {
    return simple(next, doc, (d) => {
      const target = d.items.find((i) => i.id === op.itemId)!;
      target.effects = target.effects.filter((e) => e.id !== op.effectId);
    }, [{ type: 'effect.add', itemId: op.itemId, effect: clone(effect) }]);
  }
  const before = { type: effect.type, params: clone(effect.params) };
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    const e = target.effects.find((x) => x.id === op.effectId)!;
    if (op.patch.type !== undefined) e.type = op.patch.type;
    if (op.patch.params !== undefined) e.params = { ...e.params, ...op.patch.params };
  }, [{ type: 'effect.update', itemId: op.itemId, effectId: op.effectId, patch: before }]);
}

function applyMaskOp(
  next: TimelineDoc,
  doc: TimelineDoc,
  op: Extract<Op, { type: 'mask.add' | 'mask.remove' }>,
): ApplyResult {
  const item = requireItem(doc, op.itemId);
  if (op.type === 'mask.add') {
    maskSchema.parse(op.mask);
    if (item.masks.some((m) => m.id === op.mask.id)) {
      throw new OpError(`Mask ${op.mask.id} already exists on item ${item.id}.`);
    }
    return simple(next, doc, (d) => {
      d.items.find((i) => i.id === op.itemId)!.masks.push(clone(op.mask));
    }, [{ type: 'mask.remove', itemId: op.itemId, maskId: op.mask.id }]);
  }
  const mask = item.masks.find((m) => m.id === op.maskId);
  if (!mask) throw new OpError(`Mask ${op.maskId} not found on item ${item.id}.`);
  return simple(next, doc, (d) => {
    const target = d.items.find((i) => i.id === op.itemId)!;
    target.masks = target.masks.filter((m) => m.id !== op.maskId);
  }, [{ type: 'mask.add', itemId: op.itemId, mask: clone(mask) }]);
}

// ---------- log helpers ----------

export function makeOpEntry(op: Op, seq: number, actor: OpEntry['actor']): OpEntry {
  return { seq, actor, op, createdAt: new Date().toISOString() };
}

export function effectiveSpeed(item: Item): number {
  if (item.timeRemap.length >= 2) {
    const first = item.timeRemap[0]!;
    const last = item.timeRemap[item.timeRemap.length - 1]!;
    const dur = Math.max(1, last.frame - first.frame);
    return (last.sourceFrame - first.sourceFrame) / dur;
  }
  return item.speed;
}
