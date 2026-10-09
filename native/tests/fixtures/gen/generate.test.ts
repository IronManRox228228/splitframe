/**
 * Golden fixture generator for the native port (native/tests/tst_golden.cpp).
 *
 * Runs the real TypeScript packages (packages/schema + packages/editor-core) over hand-written
 * docs and op sequences and writes what they produce as JSON next to this folder. The C++ tests
 * load the same inputs, run the same ops and must produce identical JSON (compared parsed, not as
 * text). Nothing here is random: ids and timestamps are fixed, so regenerating is a no-op unless
 * the TS behaviour changed, and the diff then shows exactly what the native side must follow.
 *
 * Run from the repo root (vitest is already installed; this installs nothing):
 *   pnpm exec vitest run --config native/tests/fixtures/gen/vitest.config.ts
 *
 * Expected docs are normalised through timelineDocSchema.parse, because the native model always
 * holds fully defaulted values (the TS apply engine stores raw `props` from item.update patches).
 */
import { mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, it } from 'vitest';
import {
  applyOp,
  applyOps,
  createEmptyDoc,
  createItem,
  docDurationFrames,
  effectiveSpeed,
  getSnapCandidates,
  itemsOnTrack,
  snapFrame,
  snapItemStart,
  sourceFrameAt,
  sourceOutFrame,
} from '../../../../packages/editor-core/src/index.ts';
import { projectBundleSchema, timelineDocSchema } from '../../../../packages/schema/src/index.ts';

const here = dirname(fileURLToPath(import.meta.url));
const out = join(here, '..');

/* eslint-disable @typescript-eslint/no-explicit-any */
type Any = any;

const J = <T,>(v: T): T => JSON.parse(JSON.stringify(v));
const norm = (doc: Any) => timelineDocSchema.parse(J(doc));
const hex = (n: number) => n.toString(16).padStart(10, '0');
const id = (prefix: string, n: number) => `${prefix}_${hex(n)}`;
const STAMP = '2026-01-01T00:00:00.000Z';

function write(rel: string, value: unknown) {
  const path = join(out, rel);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, JSON.stringify(value, null, 2) + '\n');
}

function baseDoc(fps = 30) {
  const d = createEmptyDoc({ id: 'prj_0000000001', name: 'Golden', fps });
  d.project.createdAt = STAMP;
  d.project.updatedAt = STAMP;
  return norm(d);
}

const T = { text: 'prj_0000000001_text', main: 'prj_0000000001_main', audio: 'prj_0000000001_audio' };

const style = (extra: Record<string, unknown> = {}) => ({ fontFamily: 'Inter', fontSize: 64, color: '#ffffff', ...extra });

function mk(type: string, n: number, trackId: string, startFrame: number, durationFrames: number, extra: Any = {}) {
  return createItem(type as Any, { id: id('itm', n), trackId, startFrame, durationFrames, ...extra } as Any);
}
const video = (n: number, start: number, dur: number, extra: Any = {}) =>
  mk('video', n, T.main, start, dur, { assetId: 'ast_video', sourceInFrame: 0, ...extra });
const audio = (n: number, start: number, dur: number, extra: Any = {}) =>
  mk('audio', n, T.audio, start, dur, { assetId: 'ast_music', sourceInFrame: 0, ...extra });
const add = (item: Any) => ({ type: 'item.add', item });

function errInfo(e: Any) {
  if (e && typeof e === 'object' && Array.isArray(e.issues)) return { kind: 'ZodError' };
  if (e && e.name === 'OpError') return { kind: 'OpError', message: e.message };
  return { kind: 'Other', message: String(e?.message ?? e) };
}

/** Applies each op in turn; a failing op is recorded and leaves the doc as it was. */
function runCase(name: string, initial: Any, ops: Any[]) {
  let cur = norm(initial);
  const steps: Any[] = [];
  for (const rawOp of ops) {
    const op = J(rawOp);
    let r: Any;
    try {
      r = applyOp(cur, op);
    } catch (e) {
      steps.push({ op, ok: false, error: errInfo(e) });
      continue;
    }
    {
      // an op that applies but yields a schema-invalid doc is a generator bug, not a fixture
      const after = norm(r.doc);
      let undone: Any;
      let undoError: Any;
      try {
        undone = norm(applyOps(r.doc, r.inverse, { enforceLocks: false }).doc);
      } catch (e) {
        undoError = errInfo(e);
      }
      steps.push({ op, ok: true, doc: after, inverse: J(r.inverse), undone, undoError });
      cur = after;
    }
  }
  write(`cases/${name}.json`, { name, initial: norm(initial), steps });
}

describe('golden fixtures', () => {
  it('writes them', () => {
    rmSync(join(out, 'cases'), { recursive: true, force: true });
    rmSync(join(out, 'docs'), { recursive: true, force: true });

    // ---------- docs: parse + defaults + save ----------
    const rich = baseDoc();
    const richOps: Any[] = [
      add(video(1, 0, 90, { sourceInFrame: 12, speed: 0.4, volume: 0.8, muted: true, labels: { name: 'Intro', color: '#f00' } })),
      add(audio(2, 0, 180, { volume: 0.35 })),
      add(mk('text', 3, T.text, 10, 60, { props: { text: 'Héllo 日本語 \u{1F3AC}', style: style({ strokeColor: '#000', strokeWidth: 2, backgroundColor: '#112233', align: 'left', uppercase: true, letterSpacing: 1.5, padding: 8, borderRadius: 4, fontWeight: 900, lineHeight: 1.5 }) } })),
      add(mk('caption', 4, T.text, 70, 90, { props: { words: [{ w: 'one', startMs: 0, endMs: 900, conf: 0.97, speaker: 'A' }, { w: 'two', startMs: 1000, endMs: 1900 }], style: { ...style(), highlight: 'word-bg', highlightColor: '#00ff00', placementY: 0.5, maxCharsPerLine: 20 }, mode: 'word', maxWordsPerCard: 3 } })),
      add(mk('shape', 5, T.text, 0, 30, { props: { shape: 'triangle', fill: '#abcdef', stroke: '#000', strokeWidth: 3, radius: 2, width: 200.5, height: 100 }, transform: { x: 12.5, y: -4, scale: 1.5, rotation: 45, opacity: 0.75 } })),
      add(mk('motionGraphic', 6, T.text, 30, 30, { props: { code: 'export default function X() { return null }', inputProps: { title: 'T', n: [1, 2, { deep: true }], nothing: null } } })),
      { type: 'effect.add', itemId: id('itm', 1), effect: { id: 'fx_1', type: 'blur', params: { amount: 4.5, mode: 'gauss', on: true } } },
      { type: 'mask.add', itemId: id('itm', 1), mask: { id: 'mask_1', shape: 'path', path: [{ x: 0, y: 0 }, { x: 100, y: 0 }, { x: 50, y: 80 }], feather: 3, invert: true, keyframes: { feather: [{ frame: 0, value: 0, easing: 'easeIn' }] }, tracking: { assetId: 'ast_video', status: 'done', positions: [{ frame: 0, x: 1, y: 2 }] } } },
      { type: 'item.setKeyframes', itemId: id('itm', 1), property: 'transform.x', keyframes: [{ frame: 30, value: 5, easing: 'hold' }, { frame: 0, value: 0, easing: 'easeInOut' }] },
      { type: 'item.setTimeRemap', itemId: id('itm', 1), points: [{ frame: 0, sourceFrame: 12 }, { frame: 90, sourceFrame: 300 }] },
      { type: 'marker.add', marker: { id: 'mrk_0000000001', frame: 45, label: 'Beat', color: '#f0f' } },
      { type: 'project.setStyleConfig', styleConfig: { fonts: ['Inter', 'Anton'], primaryColor: '#123456', backgroundColor: '#000000', captionStyle: { ...style(), highlight: 'none', highlightColor: '#fff', placementY: 0.9, maxCharsPerLine: 30 }, titleStyle: style({ fontSize: 120 }) } },
      { type: 'project.setReference', assetId: 'ast_ref' },
      { type: 'track.update', trackId: T.audio, patch: { muted: true, name: 'Music' } },
    ];
    let richDoc = rich;
    for (const op of richOps) richDoc = norm(applyOp(richDoc, J(op)).doc);

    const minimal = {
      project: { id: 'prj_0000000002', name: 'Sparse', createdAt: STAMP, updatedAt: STAMP },
      tracks: [{ id: 'trk_0000000001', kind: 'video', name: 'V' }, { id: 'trk_0000000002', kind: 'audio', name: 'A', locked: true }],
      items: [
        { id: 'itm_0000000001', trackId: 'trk_0000000001', type: 'video', startFrame: 5, durationFrames: 10, assetId: 'ast_x', transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 }, props: {}, futureField: { nested: 1 } },
        { id: 'itm_0000000002', trackId: 'trk_0000000002', type: 'audio', startFrame: 0, durationFrames: 30, assetId: 'ast_y', transform: { x: 0, y: 0, scale: 1, scaleX: 1, scaleY: 1, rotation: 0, opacity: 1 }, props: { fadeInFrames: 5 } },
      ],
      markers: [],
      extraTopLevel: true,
    };
    const empty = { project: { id: 'prj_0000000003', name: '', fps: 60, width: 3840, height: 2160, templateId: 'tpl_a', createdAt: '', updatedAt: '' }, tracks: [], items: [], markers: [] };
    const docs = { rich: richDoc, minimal, empty, ntsc: { ...J(minimal), project: { ...minimal.project, fps: 24, width: 1080, height: 1920 } } };
    for (const [name, input] of Object.entries(docs)) write(`docs/${name}.json`, { name, input: J(input), expected: norm(input) });

    // documents that must be rejected: zod's first issue path is what the native error must name
    const badBase = J(minimal) as Any;
    const bad = (name: string, mutate: (d: Any) => void) => {
      const d = J(badBase);
      mutate(d);
      let path = '';
      try {
        timelineDocSchema.parse(d);
      } catch (e: Any) {
        const issue = e.issues[0];
        path = issue.path.map((p: Any, i: number) => (typeof p === 'number' ? `[${p}]` : i === 0 ? p : `.${p}`)).join('');
      }
      return { name, input: d, path };
    };
    write('docs/_bad.json', [
      bad('no project name', (d) => delete d.project.name),
      bad('fractional fps', (d) => (d.project.fps = 29.97)),
      bad('zero width', (d) => (d.project.width = 0)),
      bad('track kind', (d) => (d.tracks[0].kind = 'sideways')),
      bad('duration zero', (d) => (d.items[0].durationFrames = 0)),
      bad('negative start', (d) => (d.items[0].startFrame = -1)),
      bad('speed too high', (d) => (d.items[0].speed = 17)),
      bad('scale zero', (d) => (d.items[0].transform.scale = 0)),
      bad('opacity high', (d) => (d.items[0].transform.opacity = 2)),
      bad('bad easing', (d) => (d.items[0].keyframes = { 'transform.x': [{ frame: 0, value: 1, easing: 'wobbly' }] })),
      bad('items not array', (d) => (d.items = 'x')),
      bad('null assetId', (d) => (d.items[0].assetId = null)),
      bad('fade not int', (d) => (d.items[1].props.fadeInFrames = 1.5)),
      bad('marker negative', (d) => (d.markers = [{ id: 'm', frame: -3, label: 'x' }])),
    ]);

    const bundle = {
      doc: richDoc,
      assets: [
        { id: 'ast_video', projectId: 'prj_0000000001', kind: 'video', path: 'C:/media/a.mp4', originalName: 'a.mp4', createdAt: STAMP, fps: 29.97, durationMs: 4000, width: 1920, height: 1080, hasAudio: true, sizeBytes: 123456, mtimeMs: 1700000000000.5, hash: 'abc', metadata: { codec: 'h264', location: 'Paris' }, status: 'analyzed', stage: 'done' },
        { id: 'ast_music', projectId: 'prj_0000000001', kind: 'audio', path: 'C:/media/b.mp3', originalName: 'b.mp3', createdAt: STAMP },
      ],
      transcripts: [{ assetId: 'ast_video', words: [{ w: 'hi', startMs: 0, endMs: 300, conf: 0.5, speaker: 'S' }] }],
      scenes: [{ id: 'scn_1', assetId: 'ast_video', startMs: 0, endMs: 2000, description: 'Opening', tags: ['a'], keyframePaths: ['k.jpg'] }],
      beatMaps: [{ assetId: 'ast_music', bpm: 120.5, beatsMs: [0, 500, 1000], downbeatsMs: [0], sections: [{ startMs: 0, endMs: 1000, label: 'intro' }] }],
    };
    write('docs/_bundle.json', { name: 'bundle', input: J(bundle), expected: projectBundleSchema.parse(J(bundle)) });

    // ---------- op sequences ----------
    const extraTrack = id('trk', 0xa1);
    runCase('basic_edit', baseDoc(), [
      { type: 'track.add', trackId: extraTrack, kind: 'overlay', name: 'Overlay', index: 1 },
      add(video(1, 0, 90, { sourceInFrame: 100 })),
      add(video(2, 90, 90)),
      add(audio(3, 0, 180)),
      add(mk('text', 4, T.text, 10, 60, { props: { text: 'Title', style: style() } })),
      add(mk('caption', 5, T.text, 100, 90, { props: { words: [{ w: 'one', startMs: 0, endMs: 900 }, { w: 'two', startMs: 1000, endMs: 1900 }, { w: 'three', startMs: 2000, endMs: 2900 }], style: style({ fontSize: 48 }) } })),
      add(mk('shape', 6, T.text, 0, 20, { props: { shape: 'rect', fill: '#ff0000', width: 100, height: 50 } })),
      add(mk('motionGraphic', 7, T.text, 20, 20, { props: { code: 'x', inputProps: { a: 1 } } })),
      add(mk('image', 8, extraTrack, 0, 45, { assetId: 'ast_img' })),
      { type: 'item.add', item: (() => { const v = J(video(9, 200, 30)); delete v.props; return v; })() }, // props omitted: schema defaults
      { type: 'item.add', item: (() => { const t = J(mk('text', 10, T.text, 300, 30, { props: { text: 'x', style: style() } })); delete t.props; return t; })() }, // text without props: zod error
      { type: 'item.update', itemId: id('itm', 1), patch: { transform: { x: 100.5, opacity: 0.5 }, volume: 0.25, muted: true, labels: { name: 'Hero', color: '#0f0' } } },
      { type: 'item.update', itemId: id('itm', 1), patch: { labels: { color: '#fff' } } },
      { type: 'item.update', itemId: id('itm', 1), patch: { keyframes: { 'transform.scale': [{ frame: 0, value: 1, easing: 'linear' }, { frame: 30, value: 2, easing: 'easeOut' }] } } },
      { type: 'item.update', itemId: id('itm', 1), patch: { keyframes: { 'transform.scale': [] } } },
      { type: 'item.update', itemId: id('itm', 1), patch: { effects: [{ id: 'fx', type: 'blur', params: { a: 1 } }], masks: [{ id: 'm', shape: 'rect', feather: 0, invert: false }] } },
      { type: 'item.update', itemId: id('itm', 4), patch: { props: { text: 'Changed', style: style({ fontSize: 80 }) } } },
      { type: 'item.update', itemId: id('itm', 4), patch: { props: { text: 5 } } }, // wrong shape: zod error
      { type: 'item.update', itemId: id('itm', 1), patch: { startFrame: -20, durationFrames: 0, sourceInFrame: 3 } },
      { type: 'item.update', itemId: id('itm', 8), patch: { trackId: T.audio } }, // image to audio track: rejected
      { type: 'item.move', itemId: id('itm', 2), startFrame: 120 },
      { type: 'item.move', itemId: id('itm', 8), trackId: T.main, startFrame: 300 },
      { type: 'item.move', itemId: id('itm', 8) }, // nothing to do: rejected
      { type: 'item.trim', itemId: id('itm', 1), edge: 'in', frame: 10 },
      { type: 'item.trim', itemId: id('itm', 1), edge: 'out', frame: 50 },
      { type: 'item.trim', itemId: id('itm', 1), edge: 'out', frame: 50 }, // no-op: rejected
      { type: 'item.trim', itemId: id('itm', 3), edge: 'out', frame: 150, ripple: true },
      { type: 'item.split', itemId: id('itm', 5), atFrame: 130, newItemId: id('itm', 11) },
      { type: 'item.split', itemId: id('itm', 11), atFrame: 5000, newItemId: id('itm', 12) }, // outside: rejected
      { type: 'item.split', itemId: id('itm', 4), atFrame: 40, newItemId: id('itm', 12) },
      { type: 'item.clone', itemId: id('itm', 4), newItemId: id('itm', 13) },
      { type: 'item.clone', itemId: id('itm', 6), newItemId: id('itm', 14), trackId: extraTrack, startFrame: 60 },
      { type: 'item.clone', itemId: id('itm', 4), newItemId: id('itm', 4) }, // duplicate id: rejected
      { type: 'item.slip', itemId: id('itm', 2), sourceInFrame: 40 },
      { type: 'item.slip', itemId: id('itm', 4), sourceInFrame: 40 }, // text cannot slip: rejected
      { type: 'item.setSpeed', itemId: id('itm', 2), speed: 1.5 },
      { type: 'item.setSpeed', itemId: id('itm', 4), speed: 2 }, // text cannot retime: rejected
      { type: 'item.setTimeRemap', itemId: id('itm', 2), points: [{ frame: 60, sourceFrame: 100 }, { frame: 0, sourceFrame: 0 }] },
      { type: 'item.setSpeed', itemId: id('itm', 2), speed: 0.5 }, // clears the remap
      { type: 'item.setTimeRemap', itemId: id('itm', 2), points: [{ frame: 999, sourceFrame: 1 }] }, // out of range: rejected
      { type: 'item.setKeyframes', itemId: id('itm', 2), property: 'volume', keyframes: [{ frame: 9, value: 1, easing: 'linear' }, { frame: 1, value: 0, easing: 'hold' }] },
      { type: 'item.setKeyframes', itemId: id('itm', 2), property: 'volume', keyframes: [] },
      { type: 'effect.add', itemId: id('itm', 2), effect: { id: 'fx_a', type: 'gain', params: { db: -3, mode: 'x', on: false } } },
      { type: 'effect.add', itemId: id('itm', 2), effect: { id: 'fx_a', type: 'gain', params: {} } }, // duplicate: rejected
      { type: 'effect.update', itemId: id('itm', 2), effectId: 'fx_a', patch: { type: 'eq', params: { db: 1, extra: 'y' } } },
      { type: 'effect.remove', itemId: id('itm', 2), effectId: 'fx_a' },
      { type: 'effect.remove', itemId: id('itm', 2), effectId: 'fx_a' }, // gone: rejected
      { type: 'mask.add', itemId: id('itm', 2), mask: { id: 'mk', shape: 'ellipse', feather: 2, invert: true } },
      { type: 'mask.remove', itemId: id('itm', 2), maskId: 'mk' },
      { type: 'marker.add', marker: { id: 'mrk_0000000001', frame: 45, label: 'Hit', color: '#f00' } },
      { type: 'marker.update', markerId: 'mrk_0000000001', patch: { label: 'Drop', frame: 50 } },
      { type: 'marker.remove', markerId: 'mrk_0000000001' },
      { type: 'marker.remove', markerId: 'mrk_0000000001' }, // gone: rejected
      { type: 'project.rename', name: 'Renamed' },
      { type: 'project.rename', name: '' }, // zod error
      { type: 'project.setCanvas', width: 1080, height: 1920 },
      { type: 'project.setStyleConfig', styleConfig: { fonts: ['A'], primaryColor: '#111', backgroundColor: '#222', titleStyle: style() } },
      { type: 'project.setReference', assetId: 'ast_ref' },
      { type: 'project.setReference', assetId: null },
      { type: 'track.update', trackId: extraTrack, patch: { name: 'Over', hidden: true } },
      { type: 'track.reorder', trackIds: [T.audio, extraTrack, T.main, T.text] },
      { type: 'track.reorder', trackIds: [T.audio] }, // wrong set: rejected
      { type: 'track.add', trackId: extraTrack, kind: 'video', name: 'dup' }, // duplicate: rejected
      { type: 'track.remove', trackId: extraTrack },
      { type: 'item.remove', itemIds: [id('itm', 13), id('itm', 12)] },
      { type: 'item.remove', itemIds: ['itm_nope'] }, // rejected
      { type: 'batch', ops: [{ type: 'project.rename', name: 'In batch' }, { type: 'marker.add', marker: { id: 'mrk_0000000002', frame: 5, label: 'b' } }] },
      { type: 'batch', ops: [{ type: 'marker.add', marker: { id: 'mrk_0000000003', frame: 5, label: 'c' } }, { type: 'item.update', itemId: 'itm_missing', patch: { volume: 0.5 } }] }, // atomic failure
      { type: 'batch', ops: [{ type: 'batch', ops: [{ type: 'project.rename', name: 'x' }] }] }, // nested: rejected
    ]);

    // ripple edits
    const four = baseDoc();
    runCase('ripple', four, [
      add(video(1, 0, 60)), add(video(2, 60, 60)), add(video(3, 120, 60)), add(video(4, 180, 60)),
      add(audio(5, 0, 240)),
      { type: 'item.remove', itemIds: [id('itm', 2)], ripple: true },
      { type: 'item.remove', itemIds: [id('itm', 1), id('itm', 3)], ripple: true },
      { type: 'item.remove', itemIds: [id('itm', 5)], ripple: false },
      add(video(6, 60, 60)),
      { type: 'item.trim', itemId: id('itm', 4), edge: 'in', frame: 20, ripple: true },
      { type: 'item.trim', itemId: id('itm', 4), edge: 'out', frame: 500, ripple: true },
      { type: 'item.trim', itemId: id('itm', 4), edge: 'in', frame: 9999, ripple: true },
      { type: 'item.trim', itemId: id('itm', 6), edge: 'in', frame: -50, ripple: false },
    ]);
    runCase('ripple_overlap_and_tracks', baseDoc(), [
      { type: 'track.add', trackId: id('trk', 0xb2), kind: 'video', name: 'B' },
      add(video(1, 0, 100)),
      add(mk('video', 2, id('trk', 0xb2), 50, 100, { assetId: 'ast_v', sourceInFrame: 0 })),
      add(video(3, 60, 20)), // inside item 1's span
      add(video(4, 100, 40)),
      { type: 'item.remove', itemIds: [id('itm', 1)], ripple: true },
      { type: 'item.remove', itemIds: [id('itm', 4), id('itm', 2)], ripple: true },
    ]);

    // trims and splits at fractional speeds (rounding must follow Math.round)
    runCase('fractional_speed', baseDoc(), [
      add(video(1, 0, 100, { speed: 0.4 })),
      add(video(2, 200, 77, { speed: 1.5, sourceInFrame: 33 })),
      add(video(3, 400, 50, { sourceInFrame: 0, speed: 0.25 })),
      { type: 'item.trim', itemId: id('itm', 1), edge: 'in', frame: 3 },
      { type: 'item.trim', itemId: id('itm', 2), edge: 'in', frame: 211 },
      { type: 'item.trim', itemId: id('itm', 2), edge: 'in', frame: 0 },
      { type: 'item.split', itemId: id('itm', 2), atFrame: 250, newItemId: id('itm', 20) },
      { type: 'item.split', itemId: id('itm', 3), atFrame: 421, newItemId: id('itm', 21) },
      { type: 'item.setTimeRemap', itemId: id('itm', 20), points: [{ frame: 0, sourceFrame: 10 }, { frame: 10, sourceFrame: 400 }, { frame: 20, sourceFrame: 500 }] },
      { type: 'item.split', itemId: id('itm', 20), atFrame: 260, newItemId: id('itm', 22) },
      { type: 'item.slip', itemId: id('itm', 21), sourceInFrame: -5 },
    ]);

    // frame-rate changes
    runCase('fps', baseDoc(30), [
      add(video(1, 7, 33, { props: { fadeInFrames: 5, fadeOutFrames: 9 }, keyframes: { 'transform.x': [{ frame: 3, value: 1, easing: 'linear' }] }, timeRemap: [{ frame: 0, sourceFrame: 1 }, { frame: 33, sourceFrame: 70 }] })),
      add(video(2, 40, 1)),
      add(audio(3, 11, 50, { props: { fadeInFrames: 3 } })),
      { type: 'marker.add', marker: { id: 'mrk_0000000001', frame: 31, label: 'm' } },
      { type: 'project.setFps', fps: 60 },
      { type: 'project.setFps', fps: 24 },
      { type: 'project.setFps', fps: 25 },
      { type: 'project.setFps', fps: 121 }, // zod error
      { type: 'project.setFps', fps: 1 },
    ]);

    // locked tracks
    runCase('locks', baseDoc(), [
      add(video(1, 0, 100)),
      add(audio(2, 0, 100)),
      { type: 'track.update', trackId: T.main, patch: { locked: true } },
      { type: 'item.move', itemId: id('itm', 1), startFrame: 5 },
      { type: 'item.split', itemId: id('itm', 1), atFrame: 50, newItemId: id('itm', 3) },
      { type: 'item.remove', itemIds: [id('itm', 1)] },
      { type: 'item.update', itemId: id('itm', 1), patch: { volume: 0.5 } },
      { type: 'item.clone', itemId: id('itm', 1), newItemId: id('itm', 4) },
      { type: 'item.setSpeed', itemId: id('itm', 1), speed: 2 },
      { type: 'item.trim', itemId: id('itm', 1), edge: 'out', frame: 80 },
      { type: 'effect.add', itemId: id('itm', 1), effect: { id: 'e', type: 't', params: {} } },
      add(video(5, 200, 10)),
      { type: 'track.remove', trackId: T.main },
      { type: 'item.update', itemId: id('itm', 2), patch: { trackId: T.main } }, // audio item to a locked track
      { type: 'item.clone', itemId: id('itm', 2), newItemId: id('itm', 6) }, // source on an unlocked track
      { type: 'batch', ops: [{ type: 'track.update', trackId: T.main, patch: { locked: false } }, { type: 'item.move', itemId: id('itm', 1), startFrame: 5 }] },
      { type: 'marker.add', marker: { id: 'mrk_0000000009', frame: 1, label: 'free' } },
    ]);

    // caption splitting
    runCase('caption_split', baseDoc(), [
      add(mk('caption', 1, T.text, 100, 90, { props: { words: [{ w: 'one', startMs: 0, endMs: 900, conf: 0.9 }, { w: 'two', startMs: 1000, endMs: 1900 }, { w: 'three', startMs: 2000, endMs: 2900, speaker: 'B' }], style: style() }, keyframes: { 'transform.y': [{ frame: 10, value: 1, easing: 'linear' }, { frame: 70, value: 2, easing: 'linear' }] } })),
      { type: 'item.split', itemId: id('itm', 1), atFrame: 130, newItemId: id('itm', 2) },
      { type: 'item.split', itemId: id('itm', 2), atFrame: 160, newItemId: id('itm', 3) },
      { type: 'project.setFps', fps: 24 },
      { type: 'item.split', itemId: id('itm', 3), atFrame: 150, newItemId: id('itm', 5) },
    ]);

    // ---------- query helpers: source mapping, durations, snapping ----------
    const qdoc = (() => {
      let d = baseDoc();
      const ops: Any[] = [
        add(video(1, 10, 90, { speed: 2, sourceInFrame: 5 })),
        add(video(2, 0, 60, { timeRemap: [{ frame: 0, sourceFrame: 0 }, { frame: 30, sourceFrame: 30 }, { frame: 60, sourceFrame: 30 }] })),
        add(video(3, 100, 50, { timeRemap: [{ frame: 10, sourceFrame: 100 }], sourceInFrame: 90 })),
        add(video(4, 200, 40, { timeRemap: [{ frame: 5, sourceFrame: 50 }, { frame: 25, sourceFrame: 10 }] })),
        add(video(5, 300, 60, { speed: 0.4, sourceInFrame: 7 })),
        add(audio(6, 0, 400, { speed: 1.5 })),
        add(mk('text', 7, T.text, 20, 40, { props: { text: 'x', style: style() } })),
        add(video(8, 90, 60, {})), // same start as item 3? no: 90 vs 100, interleaved
        add(video(9, 90, 10, {})), // ties on start frame with item 8: ordered by id
        { type: 'marker.add', marker: { id: 'mrk_0000000001', frame: 45, label: 'm' } },
        { type: 'marker.add', marker: { id: 'mrk_0000000002', frame: 333, label: 'n' } },
      ];
      for (const op of ops) d = norm(applyOp(d, J(op)).doc);
      return d;
    })();
    const frames = [-5, 0, 1, 7, 10, 11, 12, 15, 29, 30, 31, 45, 59, 60, 61, 70, 100, 105, 110, 111, 125, 149, 150, 205, 210, 220, 230, 240, 250, 301, 303, 310, 359, 400];
    const mapping = qdoc.items.map((it: Any) => ({
      id: it.id,
      sourceFrames: frames.map((f) => sourceFrameAt(it, f)),
      sourceOut: sourceOutFrame(it),
      effectiveSpeed: effectiveSpeed(it),
    }));
    const snapOptsList: Any[] = [{}, { includeItemEdges: false }, { includeMarkers: false }, { includePlayhead: 47 }, { includePlayhead: 0 }, { gridFrames: 25 }, { gridFrames: 1 }, { excludeItemIds: [id('itm', 1)] }, { extra: [{ frame: 77, kind: 'playhead' }] }];
    const snaps = snapOptsList.map((opts) => {
      const candidates = getSnapCandidates(qdoc, opts);
      return {
        opts,
        candidates: J(candidates),
        snapFrame: [0.5, 3, 12.4, 44, 62, 88.5, 101, 150, 151, 332, 1000].map((f) => ({ frame: f, threshold: 5, result: J(snapFrame(f, candidates, 5)) })),
        snapItemStart: qdoc.items.slice(0, 6).flatMap((it: Any) => [2, 58, 95, 205, 311, 500].map((p) => ({ itemId: it.id, proposed: p, threshold: 6, result: snapItemStart(qdoc, it.id, p, 6, opts) }))),
      };
    });
    write('queries.json', {
      doc: qdoc,
      frames,
      duration: docDurationFrames(qdoc),
      itemsOnTrack: Object.fromEntries(qdoc.tracks.map((t: Any) => [t.id, itemsOnTrack(qdoc, t.id).map((i: Any) => i.id)])),
      mapping,
      snaps,
    });
  });
});
