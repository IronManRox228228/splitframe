import type { Asset, Item, Op, TimelineDoc, Transcript } from '@cutboard/schema';

/** Everything the harness reads about the open project, in one immutable snapshot. */
export interface Snapshot {
  doc: TimelineDoc;
  assets: Asset[];
  transcripts: Transcript[];
  editor: { selection: string[]; playheadFrame: number; highlightedRange?: { startFrame: number; endFrame: number } | null };
}

/** Deterministic per-asset notes the state doc may quote (subset of FootageNotes). */
export interface AssetNotes {
  scenes: number;
  issues: string[];
  text: string[];
  quality?: number;
  integratedLufs?: number | null;
}

/**
 * The harness's hands. The app implements it over the project service and the tool registry;
 * tests implement it over an in-memory document.
 */
export interface Backend {
  snapshot(): Promise<Snapshot>;
  /** call an existing registry tool (frames and ids: the classic tool layer) */
  call(tool: string, args: unknown): Promise<unknown>;
  /** run a registry tool against a private document instead of the project (dry runs); optional */
  callOn?(tool: string, args: unknown, io: { getDoc(): TimelineDoc; applyOps(ops: Op[], label?: string): void }): Promise<unknown>;
  /** would an export with this preset overwrite an existing file? (the app always picks a fresh name) */
  exportWouldOverwrite?(preset: string): boolean;
  /** apply ops as one history entry */
  apply(ops: Op[], label: string): Promise<void>;
  silences(assetId: string): Promise<{ startMs: number; endMs: number }[] | null>;
  notes(assetId: string): AssetNotes | null;
  /** open / close the undo group a whole job shares */
  beginGroup(label: string): void;
  endGroup(): void;
  loadPlan(): unknown | null;
  savePlan(plan: unknown | null): void;
}

export type { Item };

/** One verifier finding. `fail` makes the step retry; `warn` is reported but accepted. */
export interface Finding {
  level: 'fail' | 'warn';
  message: string;
}
