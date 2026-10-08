import type { Item } from '@cutboard/schema';
import { buildHandles, isCaption, trackLabels, type HandleEntry } from './handles.ts';
import { renderPlan, type EditPlan } from './plan.ts';
import type { AssetNotes, Snapshot } from './types.ts';
import { aspectLabel, fmtDur, fmtTime } from './units.ts';

/**
 * The edit state document: a compact text summary of the project, rebuilt by code before every
 * model call. It replaces the flattened chat history, so the model always sees the timeline as
 * it is NOW, in handles and seconds (no frames, no ids).
 */

export interface StateInput {
  snap: Snapshot;
  notes?: (assetId: string) => AssetNotes | null;
  plan?: EditPlan | null;
  /** assets get a transcript excerpt line only when this is set (the judgment steps read the transcript in full anyway) */
  excerpts?: boolean;
}

const MAX_ITEMS_PER_TRACK = 30;

const range = (a: number, b: number): string => `${fmtTime(a)}–${fmtTime(b)}`;

function itemLine(e: HandleEntry, fps: number, snap: Snapshot): string {
  const i = e.item;
  const start = i.startFrame / fps;
  const dur = i.durationFrames / fps;
  const parts = [`${e.handle} "${e.name}" ${range(start, start + dur)} (${fmtDur(dur)})`];
  if ((i.type === 'video' || i.type === 'audio') && i.sourceInFrame !== undefined) {
    const srcStart = i.sourceInFrame / fps;
    parts.push(`src ${range(srcStart, srcStart + (dur * i.speed))}`);
  }
  if (i.speed !== 1) parts.push(`speed ${i.speed}x`);
  if (i.type === 'video' || i.type === 'audio') {
    const keys = i.keyframes?.['volume']?.length ?? 0;
    if (i.volume !== 1 || keys > 0) parts.push(`volume ${Number(i.volume.toFixed(2))}${keys > 0 ? ', ducked under speech' : ''}`);
    if (i.muted) parts.push('muted');
  }
  void snap;
  return parts.join(' ');
}

function captionSummary(items: Item[], fps: number): string | null {
  const cards = items.filter(isCaption) as Extract<Item, { type: 'caption' }>[];
  if (cards.length === 0) return null;
  const start = Math.min(...cards.map((c) => c.startFrame)) / fps;
  const end = Math.max(...cards.map((c) => c.startFrame + c.durationFrames)) / fps;
  return `captions: ${cards.length} cards ${range(start, end)}, font size ${cards[0]!.props.style.fontSize}`;
}

function assetLine(snap: Snapshot, a: Snapshot['assets'][number], notes: StateInput['notes'], excerpts: boolean): string {
  const bits = [`"${a.originalName}" ${a.kind} ${fmtTime(a.durationMs / 1000)}`];
  if (a.kind !== 'audio' && a.width) bits.push(`${a.width}x${a.height}`);
  const tr = snap.transcripts.find((t) => t.assetId === a.id);
  if (tr && tr.words.length > 0) {
    const first = tr.words[0]!.startMs / 1000;
    const last = tr.words[tr.words.length - 1]!.endMs / 1000;
    bits.push(`transcript ${tr.words.length} words (${range(first, last)})`);
    if (excerpts) bits.push(`starts "${tr.words.slice(0, 9).map((w) => w.w).join(' ')}..."`);
  } else if (a.hasSpeech) bits.push('speech, transcript pending');
  else if (a.kind !== 'image') bits.push('no speech');
  const n = notes?.(a.id);
  if (n) {
    const extra: string[] = [];
    if (n.scenes > 0) extra.push(`${n.scenes} scene${n.scenes === 1 ? '' : 's'}`);
    if (n.issues.length > 0) extra.push(`issues: ${n.issues.join(', ')}`);
    if (n.text.length > 0) extra.push(`on-screen text: ${n.text.slice(0, 3).map((t) => `"${t}"`).join(', ')}`);
    if (extra.length > 0) bits.push(`notes: ${extra.join('; ')}`);
  }
  return `- ${bits.join(' · ')}`;
}

export function renderStateDoc(input: StateInput): string {
  const { snap } = input;
  const { doc } = snap;
  const fps = doc.project.fps;
  const entries = buildHandles(snap);
  const labels = trackLabels(doc.tracks);
  const lengthFrames = doc.items.reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);

  const lines: string[] = [];
  lines.push(`PROJECT: ${doc.project.width}x${doc.project.height} (${aspectLabel(doc.project.width, doc.project.height)}) · ${fps} fps · length ${fmtTime(lengthFrames / fps)}`);

  const selected = entries.filter((e) => snap.editor.selection.includes(e.item.id));
  const editor = [`selected: ${selected.length ? selected.map((e) => `${e.handle} "${e.name}"`).join(', ') : 'nothing'}`, `playhead ${fmtTime(snap.editor.playheadFrame / fps)}`];
  if (snap.editor.highlightedRange) editor.push(`highlighted ${range(snap.editor.highlightedRange.startFrame / fps, snap.editor.highlightedRange.endFrame / fps)}`);
  lines.push(`EDITOR: ${editor.join('; ')}`);

  lines.push('TIMELINE (seconds on the timeline):');
  if (doc.items.length === 0) lines.push('(empty)');
  for (const track of doc.tracks) {
    const label = labels.get(track.id)!;
    const mine = entries.filter((e) => e.track.id === track.id);
    const caps = captionSummary(doc.items.filter((i) => i.trackId === track.id), fps);
    if (mine.length === 0 && !caps) continue;
    const rendered = mine.map((e) => itemLine(e, fps, snap));
    let shown = rendered;
    if (rendered.length > MAX_ITEMS_PER_TRACK) {
      const head = rendered.slice(0, MAX_ITEMS_PER_TRACK - 8);
      const tail = rendered.slice(-6);
      shown = [...head, `... ${rendered.length - head.length - tail.length} more clips ...`, ...tail];
    }
    lines.push(`${label} ${track.kind}${track.locked ? ' (locked)' : ''}:`);
    for (const l of shown) lines.push(`  ${l}`);
    if (caps) lines.push(`  ${caps}`);
  }

  lines.push('ASSETS:');
  if (snap.assets.length === 0) lines.push('(none imported)');
  for (const a of snap.assets) lines.push(assetLine(snap, a, input.notes, input.excerpts ?? true));

  if (input.plan) {
    lines.push('PLAN:');
    lines.push(renderPlan(input.plan));
  }
  return lines.join('\n');
}
