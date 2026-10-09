import { applyOps } from '@cutboard/editor-core';
import type { Item, Op, TimelineDoc } from '@cutboard/schema';
import type { FacadeTool, Outcome } from './facade.ts';
import type { Backend, Snapshot } from './types.ts';
import { fmtTime } from './units.ts';
import { docLengthFrames } from './verify.ts';

/**
 * Dry run: what a step WOULD do, computed on a copy of the document. The real project is never
 * touched. Feeds the Ask-mode preview, the plan card's per-step summaries, the destructive-edit
 * guard and the timeline highlight.
 */

export interface DocDiff {
  beforeSec: number;
  afterSec: number;
  /** video/audio clips before the change, and how many of those no longer exist after it */
  clipsBefore: number;
  clipsGone: number;
  added: number;
  removed: number;
  trimmed: number;
  moved: number;
  changed: number;
  captionsAdded: number;
  captionsRemoved: number;
  /** removed + trimmed existing items: how many clips the step really cuts into */
  clipsTouched: number;
  /** existing items the change removes, trims, moves or edits (for the timeline highlight) */
  affectedIds: string[];
}

const isMedia = (i: Item) => i.type === 'video' || i.type === 'audio';

/** The stretch of the original file a clip plays, in frames of the original. */
const sourceSpan = (i: Item): [number, number] => {
  const start = i.sourceInFrame ?? 0;
  return [start, start + Math.max(1, Math.round(i.durationFrames * i.speed))];
};

/** Share of a clip's source footage that some clip of the same file still plays in `after`. */
function footageKept(b: Item, after: Item[]): number {
  const [s0, s1] = sourceSpan(b);
  const spans = after
    .filter((a) => a.assetId === b.assetId && a.type === b.type)
    .map(sourceSpan)
    .sort((x, y) => x[0] - y[0]);
  let covered = 0;
  let cursor = s0;
  for (const [a0, a1] of spans) {
    const lo = Math.max(a0, cursor, s0);
    const hi = Math.min(a1, s1);
    if (hi > lo) {
      covered += hi - lo;
      cursor = hi;
    }
  }
  return covered / (s1 - s0);
}

/** Everything on an item except where it sits, so "same clip, other settings" is told apart from a move. */
function otherProps(i: Item): string {
  const { startFrame: _s, durationFrames: _d, sourceInFrame: _si, ...rest } = i as Item & Record<string, unknown>;
  void _s, _d, _si;
  return JSON.stringify(rest);
}

export function docDiff(before: TimelineDoc, after: TimelineDoc): DocDiff {
  const fps = before.project.fps;
  const afterById = new Map(after.items.map((i) => [i.id, i]));
  const beforeById = new Map(before.items.map((i) => [i.id, i]));
  let removed = 0;
  let trimmed = 0;
  let moved = 0;
  let changed = 0;
  let captionsRemoved = 0;
  let clipsGone = 0;
  const affected: string[] = [];
  for (const b of before.items) {
    const a = afterById.get(b.id);
    if (!a) {
      if (b.type === 'caption') captionsRemoved++;
      else removed++;
      // a rebuilt clip (cut, assembled, re-laid) has a new id but still plays the same footage: not gone
      if (isMedia(b) && (!b.assetId || footageKept(b, after.items) < 0.5)) clipsGone++;
      affected.push(b.id);
      continue;
    }
    if (a.durationFrames !== b.durationFrames || (a.sourceInFrame ?? 0) !== (b.sourceInFrame ?? 0)) {
      trimmed++;
      affected.push(b.id);
    } else if (a.startFrame !== b.startFrame || a.trackId !== b.trackId) {
      moved++;
      affected.push(b.id);
    } else if (otherProps(a) !== otherProps(b)) {
      changed++;
      affected.push(b.id);
    }
  }
  let added = 0;
  let captionsAdded = 0;
  for (const a of after.items) {
    if (beforeById.has(a.id)) continue;
    if (a.type === 'caption') captionsAdded++;
    else added++;
  }
  return {
    beforeSec: docLengthFrames(before) / fps,
    afterSec: docLengthFrames(after) / (after.project.fps || fps),
    clipsBefore: before.items.filter(isMedia).length,
    clipsGone,
    added,
    removed,
    trimmed,
    moved,
    changed,
    captionsAdded,
    captionsRemoved,
    clipsTouched: removed + trimmed,
    affectedIds: affected,
  };
}

const plural = (n: number, w: string) => `${n} ${w}${n === 1 ? '' : 's'}`;

/** "1:32 → 1:14", or empty when the length does not change. */
export function lengthChange(d: DocDiff): string {
  return Math.abs(d.afterSec - d.beforeSec) < 0.05 ? '' : `${fmtTime(d.beforeSec)} → ${fmtTime(d.afterSec)}`;
}

/** What changes, as a few counts ("2 clips cut, 14 captions added"). */
export function diffCounts(d: DocDiff): string {
  const parts: string[] = [];
  if (d.removed) parts.push(`${plural(d.removed, 'clip')} removed`);
  if (d.trimmed) parts.push(`${plural(d.trimmed, 'clip')} trimmed`);
  if (d.added) parts.push(`${plural(d.added, 'clip')} added`);
  if (d.moved) parts.push(`${plural(d.moved, 'clip')} moved`);
  if (d.changed) parts.push(`${plural(d.changed, 'clip')} changed`);
  if (d.captionsAdded) parts.push(`${plural(d.captionsAdded, 'caption')} added`);
  if (d.captionsRemoved) parts.push(`${plural(d.captionsRemoved, 'caption')} removed`);
  return parts.join(', ');
}

/** One line for a preview card: the tool's own verified description, then the timeline length. */
export function previewLine(out: Pick<Outcome, 'summary' | 'ok'>, d: DocDiff | null): string {
  const head = out.summary.split(';')[0]!.trim();
  const headline = head ? head[0]!.toUpperCase() + head.slice(1) : 'Change the timeline';
  const len = d ? lengthChange(d) : '';
  return len ? `${headline} · ${len}` : headline;
}

// ---------------------------------------------------------------- shadow backend

export interface ShadowIo {
  getDoc(): TimelineDoc;
  applyOps(ops: Op[], label?: string): void;
}

/** A backend over a private copy of the document; tools run on it unchanged. */
export class ShadowBackend implements Backend {
  private doc: TimelineDoc;
  private constructor(
    private readonly base: Backend,
    private readonly snap: Snapshot,
  ) {
    this.doc = structuredClone(snap.doc);
  }

  static async of(base: Backend): Promise<ShadowBackend> {
    return new ShadowBackend(base, await base.snapshot());
  }

  async snapshot(): Promise<Snapshot> {
    return { ...this.snap, doc: structuredClone(this.doc) };
  }

  async apply(ops: Op[]): Promise<void> {
    this.doc = applyOps(this.doc, ops).doc;
  }

  async call(tool: string, args: unknown): Promise<unknown> {
    if (!this.base.callOn) throw new Error('This backend cannot preview tools.');
    return this.base.callOn(tool, args, { getDoc: () => this.doc, applyOps: (ops) => void (this.doc = applyOps(this.doc, ops).doc) });
  }

  silences = (assetId: string) => this.base.silences(assetId);
  notes = (assetId: string) => this.base.notes(assetId);
  beginGroup(): void {}
  endGroup(): void {}
  loadPlan(): unknown | null {
    return null;
  }
  savePlan(): void {}
}

export interface Preview {
  out: Outcome;
  diff: DocDiff;
  line: string;
}

/** Run a document-editing tool on a copy; the project stays as it was. */
export async function previewTool(tool: FacadeTool, args: Record<string, unknown>, backend: Backend): Promise<Preview> {
  const shadow = await ShadowBackend.of(backend);
  const before = (await shadow.snapshot()).doc;
  const out = await tool.run(args, shadow);
  const after = (await shadow.snapshot()).doc;
  const diff = docDiff(before, after);
  return { out, diff, line: previewLine(out, diff) };
}
