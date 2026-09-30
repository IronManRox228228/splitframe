import { createEmptyDoc, createItem, Item, TimelineDoc, Track } from './index.ts';
import { newId } from '@cutboard/schema';

export { applyOp, applyOps } from './apply.ts';

export function makeDoc(): TimelineDoc {
  return createEmptyDoc({ id: newId('prj'), name: 'Test Project', fps: 30 });
}

export function trackOfKind(doc: TimelineDoc, kind: string): Track {
  const t = doc.tracks.find((x) => x.kind === kind);
  if (!t) throw new Error(`no ${kind} track`);
  return t;
}

export function videoItem(
  doc: TimelineDoc,
  startFrame: number,
  durationFrames: number,
  extra: Partial<Item> = {},
): Item {
  return createItem('video', {
    id: newId('itm'),
    trackId: trackOfKind(doc, 'video').id,
    startFrame,
    durationFrames,
    assetId: 'ast_video',
    sourceInFrame: 0,
    ...extra,
  } as never);
}

export function audioItem(
  doc: TimelineDoc,
  startFrame: number,
  durationFrames: number,
  extra: Partial<Item> = {},
): Item {
  return createItem('audio', {
    id: newId('itm'),
    trackId: trackOfKind(doc, 'audio').id,
    startFrame,
    durationFrames,
    assetId: 'ast_music',
    sourceInFrame: 0,
    ...extra,
  } as never);
}

export function textItem(
  doc: TimelineDoc,
  startFrame: number,
  durationFrames: number,
  text = 'Hello',
): Item {
  return createItem('text', {
    id: newId('itm'),
    trackId: trackOfKind(doc, 'text').id,
    startFrame,
    durationFrames,
    props: { text, style: { fontFamily: 'Inter', fontSize: 64, color: '#ffffff' } },
  } as never);
}
