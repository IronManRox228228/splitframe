import { applyOps, createEmptyDoc, createItem } from '@cutboard/editor-core';
import type { Asset, Op, TimelineDoc, Transcript } from '@cutboard/schema';
import { createToolRegistry, type ToolContext } from '@cutboard/tools';
import type { AssetNotes, Backend, Snapshot } from './types.ts';

/** In-memory project for harness tests: a real document, the real tool registry, no Electron. */

export const FPS = 30;

export function asset(id: string, name: string, kind: Asset['kind'], durationSec: number, extra: Partial<Asset> = {}): Asset {
  return {
    id,
    projectId: 'prj',
    kind,
    path: `/media/${name}`,
    originalName: name,
    status: 'analyzed',
    durationMs: Math.round(durationSec * 1000),
    width: kind === 'audio' ? 0 : 1920,
    height: kind === 'audio' ? 0 : 1080,
    hasAudio: kind !== 'image',
    hasSpeech: false,
    sizeBytes: 1000,
    mtimeMs: 0,
    metadata: {},
    createdAt: '2026-01-01T00:00:00.000Z',
    ...extra,
  } as Asset;
}

/** Whole sentences as words, 0.4 s per word, `gapSec` of silence between sentences. */
export function speechTranscript(assetId: string, sentences: string[], startSec = 0, gapSec = 1): Transcript {
  const words: Transcript['words'] = [];
  let t = startSec * 1000;
  for (const sentence of sentences) {
    for (const w of sentence.split(' ')) {
      words.push({ w, startMs: Math.round(t), endMs: Math.round(t + 400) });
      t += 400;
    }
    t += gapSec * 1000;
  }
  return { assetId, language: 'en', words } as Transcript;
}

export const ASSETS: Asset[] = [
  asset('ast_a', 'broll-a.mp4', 'video', 6),
  asset('ast_b', 'broll-b.mp4', 'video', 6),
  asset('ast_c', 'broll-c.mp4', 'video', 6),
  asset('ast_int', 'interview.mp4', 'video', 60, { hasSpeech: true }),
  asset('ast_mus', 'music.mp3', 'audio', 30),
];

export function fixtureDoc(): TimelineDoc {
  const doc = createEmptyDoc({ id: 'prj', name: 'Test', fps: FPS });
  const video = doc.tracks.find((t) => t.kind === 'video')!.id;
  const audio = doc.tracks.find((t) => t.kind === 'audio')!.id;
  const text = doc.tracks.find((t) => t.kind === 'text')!.id;
  const clip = (id: string, assetId: string, start: number, dur: number) =>
    createItem('video', { id, trackId: video, startFrame: start, durationFrames: dur, assetId, sourceInFrame: 0 } as never);
  doc.items.push(
    clip('itm_a', 'ast_a', 0, 180),
    clip('itm_b', 'ast_b', 180, 180),
    clip('itm_c', 'ast_c', 360, 180),
    createItem('audio', { id: 'itm_m', trackId: audio, startFrame: 0, durationFrames: 540, assetId: 'ast_mus', sourceInFrame: 0, volume: 0.3 } as never),
    createItem('text', {
      id: 'itm_t',
      trackId: text,
      startFrame: 0,
      durationFrames: 90,
      props: { text: 'Launch Day', style: { fontFamily: 'Geist', fontSize: 72, fontWeight: 800, color: '#fff', strokeWidth: 0, align: 'center', lineHeight: 1.2, letterSpacing: 0, uppercase: false, padding: 0, borderRadius: 0 } },
    } as never),
  );
  return doc;
}

export function fixtureSnapshot(overrides: Partial<Snapshot> = {}): Snapshot {
  return {
    doc: fixtureDoc(),
    assets: ASSETS,
    transcripts: [],
    editor: { selection: ['itm_b'], playheadFrame: 210 },
    ...overrides,
  };
}

export class FakeBackend implements Backend {
  doc: TimelineDoc;
  assets: Asset[];
  transcripts: Transcript[];
  selection: string[];
  silenceMaps: Record<string, { startMs: number; endMs: number }[]> = {};
  groups: string[] = [];
  history: { before: TimelineDoc; label: string }[] = [];
  plan: unknown = null;
  openGroup = false;
  calls: { tool: string; args: unknown }[] = [];

  constructor(snap: Snapshot = fixtureSnapshot()) {
    this.doc = snap.doc;
    this.assets = snap.assets;
    this.transcripts = snap.transcripts;
    this.selection = snap.editor.selection;
  }

  async snapshot(): Promise<Snapshot> {
    return { doc: structuredClone(this.doc), assets: this.assets, transcripts: this.transcripts, editor: { selection: this.selection, playheadFrame: 0 } };
  }

  async apply(ops: Op[], label: string): Promise<void> {
    const before = this.doc;
    this.doc = applyOps(this.doc, ops).doc;
    if (!this.openGroup) this.history.push({ before, label });
    else if (this.history.length === 0 || this.history[this.history.length - 1]!.label !== this.groups[this.groups.length - 1]) this.history.push({ before, label: this.groups[this.groups.length - 1]! });
  }

  async call(tool: string, args: unknown): Promise<unknown> {
    this.calls.push({ tool, args });
    const registry = createToolRegistry();
    const self = this;
    const ctx = {
      actor: 'builtin-agent',
      applyOps: async (ops: Op[], _actor: unknown, label?: string) => {
        await self.apply(ops, label ?? tool);
        return { inverses: [], seq: 1 };
      },
      getSnapshot: async () => ({ doc: self.doc, assets: self.assets, transcripts: self.transcripts, scenes: [], beatMaps: [], editorContext: { selection: self.selection, playheadFrame: 0 } }),
      getSilences: async (id: string) => self.silenceMaps[id] ?? null,
      startExport: async (preset: string) => ({ id: 'exp_1', status: 'queued', preset }),
      getExportStatus: async () => ({ status: 'queued' }),
      listExportPresets: async () => [],
      analyzeBeats: async () => ({ bpm: 120, beatsMs: Array.from({ length: 40 }, (_, i) => i * 500), downbeatsMs: [], sections: [{ startMs: 0, endMs: 20000, label: 'medium', energy: 0.5 }] }),
      undo: async () => {
        const last = self.history.pop();
        if (last) self.doc = last.before;
        return { steps: last ? 1 : 0, labels: last ? [last.label] : [], canUndo: self.history.length > 0, canRedo: false };
      },
      redo: async () => ({ steps: 0, labels: [], canUndo: false, canRedo: false }),
    } as unknown as ToolContext;
    return registry.call(tool, args, ctx);
  }

  async silences(assetId: string) {
    return this.silenceMaps[assetId] ?? null;
  }
  notes(): AssetNotes | null {
    return null;
  }
  beginGroup(label: string): void {
    this.openGroup = true;
    this.groups.push(label);
  }
  endGroup(): void {
    this.openGroup = false;
  }
  loadPlan(): unknown | null {
    return this.plan;
  }
  savePlan(plan: unknown | null): void {
    this.plan = plan;
  }
}
