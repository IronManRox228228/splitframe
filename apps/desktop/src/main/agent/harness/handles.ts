import type { Asset, Item, Track } from '@cutboard/schema';
import type { Snapshot } from './types.ts';

/**
 * Human handles for timeline items ("V1·2" = second clip on the first video track) and
 * resolution of everything the model may use to point at things: handle, name, "selected".
 * The model never sees item ids or frame numbers.
 */

export interface HandleEntry {
  handle: string;
  item: Item;
  track: Track;
  /** the name shown to the model: asset file name, the text of a title, ... */
  name: string;
  trackLabel: string;
}

const TRACK_PREFIX: Record<string, string> = { video: 'V', audio: 'A', text: 'T', overlay: 'O' };

export const isCaption = (item: Item): boolean => item.type === 'caption';

export function trackLabels(tracks: Track[]): Map<string, string> {
  const counts: Record<string, number> = {};
  const out = new Map<string, string>();
  for (const t of tracks) {
    const prefix = TRACK_PREFIX[t.kind] ?? 'X';
    counts[prefix] = (counts[prefix] ?? 0) + 1;
    out.set(t.id, `${prefix}${counts[prefix]}`);
  }
  return out;
}

export function itemName(item: Item, assets: Asset[]): string {
  if (item.type === 'text') return String((item.props as { text?: string }).text ?? 'text');
  if (item.type === 'caption') return 'caption';
  const asset = item.assetId ? assets.find((a) => a.id === item.assetId) : undefined;
  return asset?.originalName ?? item.labels?.name ?? item.type;
}

/** Handles for every item except caption cards (those are addressed as "the captions"). */
export function buildHandles(snap: Pick<Snapshot, 'doc' | 'assets'>): HandleEntry[] {
  const labels = trackLabels(snap.doc.tracks);
  const entries: HandleEntry[] = [];
  for (const track of snap.doc.tracks) {
    const trackLabel = labels.get(track.id)!;
    const items = snap.doc.items
      .filter((i) => i.trackId === track.id && !isCaption(i))
      .sort((a, b) => a.startFrame - b.startFrame || a.id.localeCompare(b.id));
    items.forEach((item, idx) => entries.push({ handle: `${trackLabel}·${idx + 1}`, item, track, name: itemName(item, snap.assets), trackLabel }));
  }
  return entries;
}

const stripExt = (s: string): string => s.replace(/\.[a-z0-9]{2,4}$/i, '');
const norm = (s: string): string => stripExt(s.trim().toLowerCase()).replace(/[\s_]+/g, '-');

export type Resolved<T> = { ok: true; value: T } | { ok: false; error: string };

const listHandles = (entries: HandleEntry[]): string => entries.map((e) => `${e.handle} "${e.name}"`).join(', ') || '(the timeline is empty)';

function normalizeHandle(ref: string): string | null {
  const m = /^\s*([vaot])\s*(\d+)\s*[·.:\-_ #]\s*(\d+)\s*$/i.exec(ref);
  return m ? `${m[1]!.toUpperCase()}${m[2]}·${m[3]}` : null;
}

/** One clip from a handle, a name ("intro-clip", "the title text"), or "selected". Errors list the valid handles. */
export function resolveClip(entries: HandleEntry[], editor: Snapshot['editor'], ref: string): Resolved<HandleEntry> {
  const raw = String(ref ?? '').trim();
  if (!raw) return { ok: false, error: `No clip given. Valid clips: ${listHandles(entries)}.` };
  const handle = normalizeHandle(raw);
  if (handle) {
    const hit = entries.find((e) => e.handle === handle);
    return hit ? { ok: true, value: hit } : { ok: false, error: `No clip ${handle}. Valid clips: ${listHandles(entries)}.` };
  }
  if (/^(the )?(selected|selection|current)( clip| item)?$/i.test(raw)) {
    const picked = entries.filter((e) => editor.selection.includes(e.item.id));
    if (picked.length === 1) return { ok: true, value: picked[0]! };
    if (picked.length > 1) return { ok: false, error: `${picked.length} clips are selected (${picked.map((p) => p.handle).join(', ')}); name one.` };
    return { ok: false, error: `Nothing is selected. Valid clips: ${listHandles(entries)}.` };
  }
  const q = norm(raw);
  const exact = entries.filter((e) => norm(e.name) === q);
  const pool = exact.length > 0 ? exact : entries.filter((e) => norm(e.name).includes(q));
  if (pool.length === 1) return { ok: true, value: pool[0]! };
  if (pool.length > 1) return { ok: false, error: `"${raw}" matches several clips (${pool.map((p) => p.handle).join(', ')}); use a handle.` };
  return { ok: false, error: `Unknown clip "${raw}". Valid clips: ${listHandles(entries)}.` };
}

/** Several clips; "selected" expands to the whole selection. Stops at the first bad ref. */
export function resolveClips(entries: HandleEntry[], editor: Snapshot['editor'], refs: string[]): Resolved<HandleEntry[]> {
  const out: HandleEntry[] = [];
  for (const ref of refs) {
    if (/^(the )?(selected|selection)( clips| items| clip)?$/i.test(String(ref).trim())) {
      const picked = entries.filter((e) => editor.selection.includes(e.item.id));
      if (picked.length === 0) return { ok: false, error: `Nothing is selected. Valid clips: ${listHandles(entries)}.` };
      out.push(...picked);
      continue;
    }
    const r = resolveClip(entries, editor, ref);
    if (!r.ok) return r;
    out.push(r.value);
  }
  return { ok: true, value: [...new Map(out.map((e) => [e.item.id, e])).values()] };
}

/** An imported asset by file name (extension optional), unique substring, or kind ("the music"). */
export function resolveAsset(assets: Asset[], ref: string, kind?: Asset['kind']): Resolved<Asset> {
  const pool = kind ? assets.filter((a) => a.kind === kind) : assets;
  const names = pool.map((a) => `"${a.originalName}"`).join(', ') || '(none imported)';
  const raw = String(ref ?? '').trim();
  if (!raw) return pool.length === 1 ? { ok: true, value: pool[0]! } : { ok: false, error: `No asset given. Assets: ${names}.` };
  const q = norm(raw.replace(/^the\s+/i, ''));
  const exact = pool.filter((a) => norm(a.originalName) === q);
  const near = exact.length ? exact : pool.filter((a) => norm(a.originalName).includes(q));
  if (near.length === 1) return { ok: true, value: near[0]! };
  if (near.length > 1) return { ok: false, error: `"${raw}" matches several assets (${near.map((a) => `"${a.originalName}"`).join(', ')}); use the full name.` };
  if (/music|song|track|audio|sound/i.test(raw)) {
    const audio = pool.filter((a) => a.kind === 'audio');
    if (audio.length === 1) return { ok: true, value: audio[0]! };
  }
  return { ok: false, error: `Unknown asset "${raw}". Assets: ${names}.` };
}
