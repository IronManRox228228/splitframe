import { newId, type Item, type Op } from '@cutboard/schema';
import { projectService } from '../../src/main/project-service.ts';
import { callTool } from '../../src/main/tools-bridge.ts';
import { exportService } from '../../src/main/export-service.ts';
import { resolveExport } from '../../src/shared/export-options.ts';
import { editorContextCache } from '../../src/main/editor-context.ts';
import { TraceRecorder } from '../lib/trace.ts';
import { captionItems } from '../lib/checks.ts';
import type { AgentDriver, DriverContext } from '../types.ts';

/**
 * Oracle driver: no LLM. For each task it performs the correct edit deterministically, through
 * the same tool layer the agent uses (callTool, actor 'builtin-agent'), using the fixture ground
 * truth. Running the suite with it proves every task CAN score 1.0; anything below 1.0 means a
 * check, the manifest or an app tool is wrong. One script per task id, one step per user turn.
 */

interface Env {
  ctx: DriverContext;
  call(tool: string, args: Record<string, unknown>): Promise<Record<string, any>>;
  fps: number;
  frames(sec: number): number;
  asset(name: string): string;
  items(): Item[];
}

type Script = (env: Env, turn: number) => Promise<void>;

/** Cut source-time spans (seconds of the asset) out of one timeline item with ripple, one atomic batchEdit. */
async function cutSourceSpans(env: Env, itemId: string, spans: { startSec: number; endSec: number }[]): Promise<void> {
  const item = env.items().find((i) => i.id === itemId);
  if (!item) throw new Error(`item ${itemId} not found`);
  const toFrame = (sec: number) => item.startFrame + Math.round((env.frames(sec) - (item.sourceInFrame ?? 0)) / item.speed);
  const end0 = item.startFrame + item.durationFrames;
  const ops: Op[] = [];
  let currentEnd = end0;
  for (const span of [...spans].sort((a, b) => b.startSec - a.startSec)) {
    const spanStart = Math.max(item.startFrame, toFrame(span.startSec));
    const spanEnd = Math.min(currentEnd, toFrame(span.endSec));
    if (spanEnd <= spanStart) continue;
    let mid = item.id;
    if (spanStart > item.startFrame) {
      mid = newId('itm');
      ops.push({ type: 'item.split', itemId: item.id, atFrame: spanStart, newItemId: mid });
    }
    if (spanEnd < currentEnd) ops.push({ type: 'item.split', itemId: mid, atFrame: spanEnd, newItemId: newId('itm') });
    ops.push({ type: 'item.remove', itemIds: [mid], ripple: true });
    currentEnd = spanStart;
  }
  await env.call('batchEdit', { label: 'oracle cut', ops });
}

const interviewItem = (env: Env): Item => {
  const id = env.asset('interview.mp4');
  const it = env.items().find((i) => i.assetId === id && i.type === 'video');
  if (!it) throw new Error('interview not on the timeline');
  return it;
};

const durationFrames = (env: Env): number => env.items().reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);

const SCRIPTS: Record<string, Script> = {
  'add-title': async (e) => {
    await e.call('addText', { text: 'Launch Day', startFrame: 0, durationFrames: e.frames(3) });
  },

  'trim-clip': async (e) => {
    const clip = e.items().find((i) => i.type === 'video')!;
    await e.call('trimItem', { itemId: clip.id, edge: 'out', frame: clip.startFrame + e.frames(4) });
  },

  'delete-selected': async (e) => {
    const selection = (editorContextCache.get() as { selection?: string[] }).selection ?? [];
    await e.call('deleteItems', { itemIds: selection, ripple: true });
  },

  'reorder-clips': async (e) => {
    const byAsset = (n: string) => e.items().find((i) => i.assetId === e.asset(n))!;
    const [a, b, c] = ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4'].map(byAsset) as [Item, Item, Item];
    // park everything out of the way, then lay the clips down back to back as C, A, B
    await e.call('moveItem', { itemId: a.id, startFrame: e.frames(100) });
    await e.call('moveItem', { itemId: b.id, startFrame: e.frames(120) });
    await e.call('moveItem', { itemId: c.id, startFrame: 0 });
    await e.call('moveItem', { itemId: a.id, startFrame: c.durationFrames });
    await e.call('moveItem', { itemId: b.id, startFrame: c.durationFrames + a.durationFrames });
  },

  'vertical-reels': async (e) => {
    await e.call('batchEdit', { label: 'vertical 9:16', ops: [{ type: 'project.setCanvas', width: 1080, height: 1920 }] });
  },

  'add-captions': async (e) => {
    await e.call('addCaptions', {});
  },

  'remove-pauses': async (e) => {
    // removeSilences works from ASR word gaps, and Whisper stretches word ends across pauses
    // (see the report), so cut the real silences from the fixture ground truth instead
    await cutSourceSpans(e, interviewItem(e).id, e.ctx.manifest.fixtures['interview.mp4'].pauses);
  },

  'remove-retake': async (e) => {
    const { abandoned } = e.ctx.manifest.fixtures['interview.mp4'].retake;
    await cutSourceSpans(e, interviewItem(e).id, [abandoned]);
  },

  'remove-fillers': async (e) => {
    await cutSourceSpans(e, interviewItem(e).id, e.ctx.manifest.fixtures['interview.mp4'].fillers);
  },

  'music-under-speech': async (e) => {
    const truth = e.ctx.manifest.fixtures['interview.mp4'];
    await e.call('addAudio', { assetId: e.asset('music.mp3'), startFrame: 0, volume: 0.4 });
    const music = e.items().find((i) => i.assetId === e.asset('music.mp3'))!;
    // duckMusic ducks one flat span per item (see the report): write the envelope from the speech runs
    const runs: { start: number; end: number }[] = [];
    for (const s of truth.segments.filter((x) => x.kind !== 'pause')) {
      const last = runs[runs.length - 1];
      if (last && s.startSec - last.end < 0.5) last.end = s.endSec;
      else runs.push({ start: s.startSec, end: s.endSec });
    }
    const hi = 0.4;
    const lo = 0.12;
    const ramp = 5;
    const kf: { frame: number; value: number; easing: 'linear' }[] = [];
    for (const r of runs) {
      const s0 = e.frames(r.start);
      const s1 = e.frames(r.end);
      if (s0 - ramp > (kf[kf.length - 1]?.frame ?? -1)) kf.push({ frame: s0 - ramp, value: hi, easing: 'linear' });
      kf.push({ frame: Math.max(s0, (kf[kf.length - 1]?.frame ?? -1) + 1), value: lo, easing: 'linear' }, { frame: s1, value: lo, easing: 'linear' }, { frame: s1 + ramp, value: hi, easing: 'linear' });
    }
    await e.call('setKeyframes', { itemId: music.id, property: 'volume', keyframes: kf });
  },

  'cut-to-beat': async (e) => {
    // beatSync uses the detector's grid (121 BPM, ~0.2 s late; see the report), so place the cuts on
    // the true 120 BPM grid: a new shot every 2 s (4 beats), cycling through the three clips
    const shots: [string, number][] = [['a', 0], ['b', 0], ['c', 0], ['a', 2], ['b', 2], ['c', 2], ['b', 4], ['c', 4]];
    const SHOT_SEC = 2;
    for (const [i, [clip, srcSec]] of shots.entries()) {
      await e.call('addClip', { assetId: e.asset(`broll-${clip}.mp4`), startFrame: e.frames(i * SHOT_SEC), sourceInFrame: e.frames(srcSec), durationFrames: e.frames(SHOT_SEC) });
    }
  },

  'captions-bigger-undo': async (e, turn) => {
    if (turn === 0) {
      await e.call('addCaptions', {});
    } else if (turn === 1) {
      for (const c of captionItems(projectService.doc)) {
        await e.call('updateItem', { itemId: c.id, patch: { props: { ...c.props, style: { ...c.props.style, fontSize: Math.round(c.props.style.fontSize * 1.4) } } } });
      }
    } else {
      // the agent has no undo tool: undoing means putting the previous values back
      for (const c of captionItems(projectService.doc)) {
        await e.call('updateItem', { itemId: c.id, patch: { props: { ...c.props, style: { ...c.props.style, fontSize: Math.round(c.props.style.fontSize / 1.4) } } } });
      }
    }
  },

  'export-720p': async (e) => {
    const presets = (await e.call('listExportPresets', {})) as unknown as { name: string; height: number }[];
    const tool = presets.find((p) => p.height === 720);
    if (tool) {
      await e.call('exportVideo', { preset: tool.name });
      return;
    }
    // no agent-reachable 720p preset (see the report): take the editor's own export path
    const { width, height } = projectService.doc.project;
    await exportService.start(resolveExport('720p', 'mp4', width, height));
  },

  'customer-teaser': async (e) => {
    const topic = e.ctx.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'customer')!;
    const talk = e.items().find((i) => i.assetId === e.asset('talk.mp4'))!;
    await e.call('deleteItems', { itemIds: [talk.id] });
    await e.call('addClip', { assetId: e.asset('talk.mp4'), startFrame: 0, sourceInFrame: e.frames(topic.startSec + 1), durationFrames: e.frames(30) });
  },

  'compound-edit': async (e) => {
    const truth = e.ctx.manifest.fixtures['interview.mp4'];
    await cutSourceSpans(e, interviewItem(e).id, [...truth.pauses, truth.retake.abandoned]);
    await e.call('addCaptions', {});
    await e.call('addAudio', { assetId: e.asset('music.mp3'), startFrame: 0, durationFrames: durationFrames(e), volume: 0.3 });
    await e.call('addText', { text: 'Launch Day', startFrame: 0, durationFrames: e.frames(3) });
  },
};

export const oracleDriver: AgentDriver = {
  id: 'oracle',
  offline: true,
  async runTurn(message, _history, ctx) {
    const recorder = new TraceRecorder(message);
    const script = SCRIPTS[ctx.taskId];
    if (!script) return recorder.finish({ error: `oracle has no script for task ${ctx.taskId}` });
    let n = 0;
    const env: Env = {
      ctx,
      fps: ctx.fps,
      frames: (sec) => Math.round(sec * ctx.fps),
      asset: (name) => {
        const id = ctx.assetIds[name];
        if (!id) throw new Error(`fixture ${name} was not imported`);
        return id;
      },
      items: () => projectService.doc.items as Item[],
      async call(tool, args) {
        const callId = `oracle-${n++}`;
        recorder.emit('chat:tool', { phase: 'call', callId, tool, args });
        try {
          const result = await callTool(tool, args, 'builtin-agent');
          recorder.emit('chat:tool', { phase: 'result', callId, tool, result });
          return result as Record<string, any>;
        } catch (err) {
          recorder.emit('chat:tool', { phase: 'result', callId, tool, error: err instanceof Error ? err.message : String(err) });
          throw err;
        }
      },
    };
    try {
      await script(env, turnIndex(ctx.chatId));
      return recorder.finish();
    } catch (err) {
      return recorder.finish({ error: err instanceof Error ? err.message : String(err) });
    }
  },
};

/** chatId ends in `-<turn index>` (see run.ts) */
function turnIndex(chatId: string): number {
  const m = /-(\d+)$/.exec(chatId);
  return m ? Number(m[1]) : 0;
}
