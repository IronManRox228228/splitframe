import { newId, type Item, type Op } from '@cutboard/schema';
import { projectService } from '../../src/main/project-service.ts';
import { callTool } from '../../src/main/tools-bridge.ts';
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

/** Park the clips out of the way, then lay them back to back in this order (clip lengths are kept). */
async function layOut(e: Env, order: string[]): Promise<void> {
  const clips = order.map((n) => e.items().find((i) => i.assetId === e.asset(n))!);
  for (const [k, c] of clips.entries()) await e.call('moveItem', { itemId: c.id, startFrame: e.frames(200 + 20 * k) });
  let at = 0;
  for (const c of clips) {
    await e.call('moveItem', { itemId: c.id, startFrame: at });
    at += c.durationFrames;
  }
}

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
    await e.call('setProjectSettings', { aspect: '9:16' });
  },

  'add-captions': async (e) => {
    await e.call('addCaptions', {});
  },

  'remove-pauses': async (e) => {
    await e.call('removeSilences', { thresholdSec: 0.5 });
  },

  'remove-retake': async (e) => {
    const { abandoned } = e.ctx.manifest.fixtures['interview.mp4'].retake;
    await cutSourceSpans(e, interviewItem(e).id, [abandoned]);
  },

  'remove-fillers': async (e) => {
    await cutSourceSpans(e, interviewItem(e).id, e.ctx.manifest.fixtures['interview.mp4'].fillers);
  },

  'music-under-speech': async (e) => {
    await e.call('addAudio', { assetId: e.asset('music.mp3'), startFrame: 0, volume: 0.4 });
    const music = e.items().find((i) => i.assetId === e.asset('music.mp3'))!;
    await e.call('duckMusic', { musicItemId: music.id });
  },

  'cut-to-beat': async (e) => {
    const music = e.items().find((i) => i.assetId === e.asset('music.mp3'))!;
    await e.call('analyzeBeats', { assetId: e.asset('music.mp3') });
    await e.call('beatSync', { musicItemId: music.id });
  },

  'captions-bigger-undo': async (e, turn) => {
    if (turn === 0) {
      await e.call('addCaptions', {});
    } else if (turn === 1) {
      // one batch = one history step, so the next turn's single undo reverts all of it
      const ops: Op[] = captionItems(projectService.doc).map((c) => ({
        type: 'item.update',
        itemId: c.id,
        patch: { props: { ...c.props, style: { ...c.props.style, fontSize: Math.round(c.props.style.fontSize * 1.4) } } },
      }));
      await e.call('batchEdit', { label: 'bigger captions', ops });
    } else {
      await e.call('undo', { steps: 1 });
    }
  },

  'export-720p': async (e) => {
    await e.call('exportVideo', { preset: '720p' });
  },

  'customer-teaser': async (e) => {
    const topic = e.ctx.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'customer')!;
    const talk = e.items().find((i) => i.assetId === e.asset('talk.mp4'))!;
    await e.call('deleteItems', { itemIds: [talk.id] });
    await e.call('addClip', { assetId: e.asset('talk.mp4'), startFrame: 0, sourceInFrame: e.frames(topic.startSec + 1), durationFrames: e.frames(30) });
  },

  'compound-edit': async (e) => {
    await e.call('buildRoughCut', {});
    await e.call('addCaptions', {});
    await e.call('addAudio', { assetId: e.asset('music.mp3'), startFrame: 0, durationFrames: durationFrames(e), volume: 0.3 });
    await e.call('addText', { text: 'Launch Day', startFrame: 0, durationFrames: e.frames(3) });
  },

  // ---- held-out tasks ----
  'ho-dead-air': async (e) => {
    await e.call('removeSilences', { thresholdSec: 0.5 });
  },

  'ho-ums-ahs': async (e) => {
    const fillers = e.ctx.manifest.fixtures['interview.mp4'].fillers.filter((f) => f.word === 'um' || f.word === 'uh');
    await cutSourceSpans(e, interviewItem(e).id, fillers);
  },

  'ho-flub': async (e) => {
    const { abandoned } = e.ctx.manifest.fixtures['interview.mp4'].retake;
    await cutSourceSpans(e, interviewItem(e).id, [abandoned]);
  },

  'ho-shorten-c': async (e) => {
    const clip = e.items().find((i) => i.type === 'video')!;
    await e.call('trimItem', { itemId: clip.id, edge: 'out', frame: clip.startFrame + e.frames(4) });
  },

  'ho-square': async (e) => {
    await e.call('setProjectSettings', { aspect: '1:1' });
  },

  'ho-render-720': async (e) => {
    await e.call('exportVideo', { preset: '720p MP4' });
  },

  'ho-b-first': async (e) => {
    await layOut(e, ['broll-b.mp4', 'broll-a.mp4', 'broll-c.mp4']);
  },

  'ho-a-after-c': async (e) => {
    await layOut(e, ['broll-b.mp4', 'broll-c.mp4', 'broll-a.mp4']);
  },

  'ho-split-delete': async (e) => {
    const it = interviewItem(e);
    const at = it.startFrame + e.frames(20);
    const res = await e.call('splitItem', { itemId: it.id, atFrame: at });
    const second = (res['items'] as Item[]).find((i) => i.startFrame >= at && i.id !== it.id);
    await e.call('deleteItems', { itemIds: [second!.id], ripple: false });
  },

  'ho-thanks-title': async (e) => {
    const total = durationFrames(e);
    await e.call('addText', { text: 'Thanks for watching', startFrame: total - e.frames(3), durationFrames: e.frames(3) });
  },

  'ho-music-louder': async (e) => {
    const music = e.items().find((i) => i.assetId === e.asset('music.mp3'))!;
    await e.call('updateItem', { itemId: music.id, patch: { volume: 0.35 } });
  },

  'ho-highlight': async (e) => {
    const topic = e.ctx.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'customer')!;
    const talk = e.items().find((i) => i.assetId === e.asset('talk.mp4'))!;
    await e.call('deleteItems', { itemIds: [talk.id] });
    await e.call('addClip', { assetId: e.asset('talk.mp4'), startFrame: 0, sourceInFrame: e.frames(topic.startSec + 1), durationFrames: e.frames(15) });
  },

  // ---- held-out set 2 ----
  'h2-gaps': async (e) => {
    await e.call('removeSilences', { thresholdSec: 0.5 });
  },

  'h2-fillers': async (e) => {
    await cutSourceSpans(e, interviewItem(e).id, e.ctx.manifest.fixtures['interview.mp4'].fillers);
  },

  'h2-last-5s': async (e) => {
    const total = e.ctx.manifest.fixtures['interview.mp4'].durationSec;
    await cutSourceSpans(e, interviewItem(e).id, [{ startSec: total - 5, endSec: total }]);
  },

  'h2-drop-label-b': async (e) => {
    const b = e.items().find((i) => i.assetId === e.asset('broll-b.mp4'))!;
    await e.call('deleteItems', { itemIds: [b.id], ripple: true });
  },

  'h2-open-c': async (e) => {
    await layOut(e, ['broll-c.mp4', 'broll-a.mp4', 'broll-b.mp4']);
  },

  'h2-yellow-captions': async (e) => {
    await e.call('addCaptions', {});
    const ops: Op[] = captionItems(projectService.doc).map((c) => ({
      type: 'item.update',
      itemId: c.id,
      patch: { props: { ...c.props, style: { ...c.props.style, color: '#FFE600' } } },
    }));
    await e.call('batchEdit', { label: 'yellow captions', ops });
  },

  'h2-slow-title': async (e) => {
    const b = e.items().find((i) => i.assetId === e.asset('broll-b.mp4'))!;
    await e.call('updateItem', { itemId: b.id, patch: { speed: 0.5, durationFrames: b.durationFrames * 2 } });
    await layOut(e, ['broll-a.mp4', 'broll-b.mp4', 'broll-c.mp4']);
    await e.call('addText', { text: 'Summer Sale', startFrame: 0, durationFrames: e.frames(3) });
  },

  'h2-music-half': async (e) => {
    const music = e.items().find((i) => i.assetId === e.asset('music.mp3'))!;
    await e.call('updateItem', { itemId: music.id, patch: { volume: 0.4 } });
  },

  'h2-lose-2s': async (e) => {
    const clip = e.items().find((i) => i.type === 'video')!;
    await e.call('trimItem', { itemId: clip.id, edge: 'in', frame: clip.startFrame + e.frames(2) });
    const trimmed = e.items().find((i) => i.id === clip.id)!;
    if (trimmed.startFrame !== 0) await e.call('moveItem', { itemId: clip.id, startFrame: 0 });
  },

  'h2-vertical-captions': async (e) => {
    await e.call('setProjectSettings', { aspect: '9:16' });
    await e.call('addCaptions', {});
  },

  'h2-demo-20s': async (e) => {
    const topic = e.ctx.manifest.fixtures['talk.mp4'].topics.find((t) => t.id === 'demo')!;
    const talk = e.items().find((i) => i.assetId === e.asset('talk.mp4'))!;
    await e.call('deleteItems', { itemIds: [talk.id] });
    await e.call('addClip', { assetId: e.asset('talk.mp4'), startFrame: 0, sourceInFrame: e.frames(topic.startSec + 1), durationFrames: e.frames(20) });
  },

  'h2-cut-range': async (e) => {
    await cutSourceSpans(e, interviewItem(e).id, [{ startSec: 10, endSec: 20 }]);
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
