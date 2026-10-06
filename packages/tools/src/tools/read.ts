import { z } from 'zod';
import { frameSchema } from '@cutboard/schema';
import type { ToolDef } from '../registry.ts';

/** Context / view / search tools (main prompt §6, first batch). */

export const getEditorContext: ToolDef = {
  name: 'getEditorContext',
  description:
    'Read what the user is looking at right now: selected item ids, playhead time (frames + seconds), and any highlighted timeline range. Use it whenever the user says "the selected clip", "at this point", or "in this section".',
  input: z.object({}),
  mutates: false,
  async handler(_input, ctx) {
    const snap = (await ctx.getSnapshot()) as { editorContext?: unknown };
    return snap.editorContext ?? { selection: [], playheadFrame: 0 };
  },
};

export const listAssets: ToolDef = {
  name: 'listAssets',
  description:
    'List every asset in the project with id, kind, duration, dimensions, and analysis status. Call this before proposing any edit so you reference real asset ids.',
  input: z.object({}),
  mutates: false,
  async handler(_input, ctx) {
    const snap = (await ctx.getSnapshot()) as { assets?: unknown[] };
    return { assets: snap.assets ?? [] };
  },
};

export const getAsset: ToolDef = {
  name: 'getAsset',
  description: 'Get one asset in full: path metadata, duration, width/height, fps, audio/speech flags, analysis status.',
  input: z.object({ assetId: z.string().describe('Asset id from listAssets') }),
  mutates: false,
  async handler(input, ctx) {
    const snap = (await ctx.getSnapshot()) as { assets?: { id: string }[] };
    const asset = (snap.assets ?? []).find((a) => a.id === input.assetId);
    if (!asset) throw new Error(`Asset ${input.assetId} not found. Call listAssets to see available ids.`);
    return asset;
  },
};

export const getTimeline: ToolDef = {
  name: 'getTimeline',
  description:
    'Read the timeline. Use mode "summary" for structure (tracks + items with timing), "detail" to include transforms/effects/props. ALWAYS call this after editing to verify your changes landed — never claim success without checking.',
  input: z.object({
    mode: z.enum(['summary', 'detail']).default('summary'),
  }),
  mutates: false,
  async handler(input, ctx) {
    const snap = (await ctx.getSnapshot()) as { doc?: { tracks: unknown[]; items: unknown[]; markers: unknown[]; project: { fps: number; width: number; height: number; name: string } } };
    const doc = snap.doc;
    if (!doc) throw new Error('No project open.');
    if (input.mode === 'detail') {
      return doc;
    }
    return {
      project: doc.project,
      tracks: doc.tracks,
      items: doc.items,
      markers: doc.markers,
    };
  },
};

export const getTranscript: ToolDef = {
  name: 'getTranscript',
  description:
    'Get the word-level transcript of an asset (word, startMs, endMs). Available once the asset has been analyzed and transcription ran; otherwise returns an empty word list with a clear status.',
  input: z.object({ assetId: z.string() }),
  mutates: false,
  async handler(input, ctx) {
    const snap = (await ctx.getSnapshot()) as { transcripts?: { assetId: string; words: unknown[] }[]; assets?: { id: string; status: string }[] };
    const transcript = (snap.transcripts ?? []).find((t) => t.assetId === input.assetId);
    const asset = (snap.assets ?? []).find((a) => a.id === input.assetId);
    return {
      assetId: input.assetId,
      status: asset?.status ?? 'missing',
      words: transcript?.words ?? [],
      note: transcript ? undefined : 'No transcript for this asset: it has no speech, is still being analyzed, or transcription is unavailable (no whisper model installed).',
    };
  },
};

export const searchTranscript: ToolDef = {
  name: 'searchTranscript',
  description: 'Search what was said: phrase (case-insensitive) → matching word ranges with timestamps. Returns clip + time ranges, not just files.',
  input: z.object({
    phrase: z.string().min(1),
    assetId: z.string().optional().describe('Limit to one asset; default searches the whole project'),
  }),
  mutates: false,
  async handler(input, ctx) {
    const snap = (await ctx.getSnapshot()) as { transcripts?: { assetId: string; words: { w: string; startMs: number; endMs: number }[] }[] };
    const needle = input.phrase.toLowerCase();
    const results: { assetId: string; startMs: number; endMs: number; text: string }[] = [];
    for (const transcript of snap.transcripts ?? []) {
      if (input.assetId && transcript.assetId !== input.assetId) continue;
      const words = transcript.words;
      for (let i = 0; i < words.length; i++) {
        let j = i;
        let text = '';
        while (j < words.length) {
          text = (text + ' ' + words[j]!.w).trim();
          if (text.toLowerCase().includes(needle)) {
            results.push({ assetId: transcript.assetId, startMs: words[i]!.startMs, endMs: words[j]!.endMs, text });
            break;
          }
          if (text.length > needle.length * 3 + 20) break;
          j++;
        }
      }
    }
    return { query: input.phrase, matches: results.slice(0, 50) };
  },
};

export const captureFrame: ToolDef = {
  name: 'captureFrame',
  description:
    'Render the exact frame at a timeline position and return it as an image. Use it to SEE the footage before/after edits (e.g. to find precise cut points or check caption placement). Frames come from the same renderer as the export.',
  input: z.object({
    frame: frameSchema.describe('Timeline frame (project fps)'),
    width: z.number().int().positive().optional().describe('Optional output width override'),
  }),
  mutates: false,
  async handler(input, ctx) {
    const png = await ctx.captureFrame(input.frame, input.width);
    return { format: 'image/png', frame: input.frame, base64: png.toString('base64') };
  },
};

/** Mirrors @cutboard/renderer's effect registry (kept inline to avoid a dependency). */
const EFFECT_LIST = [
  { type: 'brightness', label: 'Brightness', params: { amount: { min: -1, max: 1, default: 0, step: 0.01 } } },
  { type: 'contrast', label: 'Contrast', params: { amount: { min: 0, max: 3, default: 1, step: 0.01 } } },
  { type: 'saturation', label: 'Saturation', params: { amount: { min: 0, max: 3, default: 1, step: 0.01 } } },
  { type: 'blur', label: 'Gaussian Blur', params: { radiusPx: { min: 0, max: 100, default: 0, step: 0.5 } } },
  { type: 'hueRotate', label: 'Hue Rotate', params: { degrees: { min: -180, max: 180, default: 0, step: 1 } } },
  { type: 'grayscale', label: 'Grayscale', params: { amount: { min: 0, max: 1, default: 0, step: 0.01 } } },
  { type: 'sepia', label: 'Sepia', params: { amount: { min: 0, max: 1, default: 0, step: 0.01 } } },
];

export const listEffects: ToolDef = {
  name: 'listEffects',
  description: 'List available effects with their parameter names, ranges, and defaults (brightness, contrast, saturation, blur, …).',
  input: z.object({}),
  mutates: false,
  async handler() {
    return { effects: EFFECT_LIST };
  },
};

export const READ_TOOLS: ToolDef[] = [
  getEditorContext,
  listAssets,
  getAsset,
  getTimeline,
  getTranscript,
  searchTranscript,
  captureFrame,
  listEffects,
];
