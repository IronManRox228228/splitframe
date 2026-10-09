import { mkdirSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join } from 'node:path';
import type { Asset, TimelineDoc, Transcript } from '@cutboard/schema';
import { getDb } from '../../src/main/db.ts';
import { projectService } from '../../src/main/project-service.ts';
import { importFiles, getAssets, getTranscriptsForProject } from '../../src/main/asset-service.ts';
import { callTool } from '../../src/main/tools-bridge.ts';
import { editorContextCache } from '../../src/main/editor-context.ts';
import { saveSettings } from '../../src/main/settings.ts';
import { ffprobe } from '../../src/main/ffmpeg.ts';
import { MAX_AGENT_STEPS } from '../../src/main/agent/chat.ts';
import { selectTasks } from '../tasks/index.ts';
import { loadManifest, type FixtureName } from '../lib/manifest.ts';
import { historyAfter, traceStats, type TraceStats } from '../lib/trace.ts';
import { DRIVERS } from './drivers.ts';
import type { AgentDriver, Check, CheckInput, ExportInfo, HistoryTurn, SetupContext, Task, TurnTrace } from '../types.ts';

/** Run configuration written by evals/run.mjs (path in CUTBOARD_EVAL). */
export interface RunConfig {
  label: string;
  driver: string;
  tasks?: string[];
  runs: number;
  provider: string;
  model: string;
  llamacppUrl: string;
  evalsDir: string;
  resultsPath: string;
  /** default per-task wall-clock budget */
  taskTimeoutMs: number;
  /** agent mode for the harness driver (default auto) */
  mode?: 'plan' | 'ask' | 'default' | 'auto';
}

export interface TaskResult {
  id: string;
  title: string;
  difficulty: string;
  run: number;
  attempts: number;
  score: number;
  checks: Check[];
  stats: TraceStats;
  turns: TurnTrace[];
  timedOut: boolean;
  /** problems with the harness itself (import/analysis/setup/check crashed), not the agent */
  infraError?: string;
  analysisMs: number;
  transcripts: Record<string, string>;
  assetNotes: string[];
  finalItems: unknown[];
  project: { width: number; height: number; fps: number };
  exports: ExportInfo[];
}

/** The asset service returns words untyped; analysis writes them in the schema's shape. */
const projectTranscripts = (projectId: string) => getTranscriptsForProject(projectId) as Transcript[];

const log = (msg: string) => process.stderr.write(`[eval] ${msg}\n`);
const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));
const CONNECTION_ERROR = /ECONNREFUSED|fetch failed|ECONNRESET|ENOTFOUND|socket hang up|Cannot connect|UND_ERR/i;

async function waitForServer(url: string, maxMs = 10 * 60_000): Promise<void> {
  const t0 = Date.now();
  for (;;) {
    try {
      const res = await fetch(`${url.replace(/\/+$/, '')}/health`);
      const body = (await res.json().catch(() => ({}))) as { status?: string };
      if (res.ok && body.status === 'ok') return;
    } catch {
      /* not up yet */
    }
    if (Date.now() - t0 > maxMs) throw new Error(`llama-server at ${url} did not become healthy`);
    await sleep(3000);
  }
}

async function waitForAssets(ids: string[], maxMs: number): Promise<void> {
  const t0 = Date.now();
  for (;;) {
    const assets = getAssets(projectService.projectId).filter((a) => ids.includes(a.id));
    if (assets.length === ids.length && assets.every((a) => a.status === 'analyzed' || a.status === 'failed')) return;
    if (Date.now() - t0 > maxMs) throw new Error('timed out waiting for asset analysis');
    await sleep(500);
  }
}

function compactItems(doc: TimelineDoc): unknown[] {
  return doc.items.map((i) => ({
    id: i.id,
    type: i.type,
    trackId: i.trackId,
    startFrame: i.startFrame,
    durationFrames: i.durationFrames,
    ...(i.assetId ? { assetId: i.assetId, sourceInFrame: i.sourceInFrame } : {}),
    ...(i.speed !== 1 ? { speed: i.speed } : {}),
    volume: i.volume,
    ...(i.keyframes?.['volume']?.length ? { volumeKeyframes: i.keyframes['volume'].length } : {}),
    ...(i.type === 'text' ? { text: i.props.text } : {}),
    ...(i.type === 'caption' ? { captionWords: i.props.words.length, fontSize: i.props.style.fontSize } : {}),
  }));
}

async function collectExports(projectId: string): Promise<ExportInfo[]> {
  const db = getDb();
  // an export started by the agent keeps running after its turn ends: wait for it to settle
  for (let i = 0; i < 600; i++) {
    const open = db.prepare(`SELECT COUNT(*) AS n FROM exports WHERE project_id=? AND status IN ('queued','rendering','encoding')`).get(projectId) as { n: number };
    if (open.n === 0) break;
    await sleep(500);
  }
  const rows = db.prepare(`SELECT id, status, output_path, error FROM exports WHERE project_id=? ORDER BY created_at`).all(projectId) as {
    id: string;
    status: string;
    output_path: string | null;
    error: string | null;
  }[];
  const out: ExportInfo[] = [];
  for (const r of rows) {
    const exists = Boolean(r.output_path && existsSync(r.output_path));
    const info: ExportInfo = { id: r.id, status: r.status, outputPath: r.output_path, exists, sizeBytes: exists ? statSync(r.output_path!).size : 0, error: r.error };
    if (exists) {
      try {
        const probe = await ffprobe(r.output_path!);
        info.width = probe.width;
        info.height = probe.height;
      } catch {
        /* unreadable output: leave dimensions unset */
      }
    }
    out.push(info);
  }
  return out;
}

async function runTask(task: Task, run: number, cfg: RunConfig, driver: AgentDriver, manifest: ReturnType<typeof loadManifest>): Promise<TaskResult> {
  const fixturesDir = join(cfg.evalsDir, 'fixtures', 'out');
  editorContextCache.clear();
  const project = projectService.create(`eval-${task.id}-${run}`);
  projectService.open(project.id);
  editorContextCache.set({ openProjectId: project.id });
  const fps = projectService.doc.project.fps;
  const result: Partial<TaskResult> = { id: task.id, title: task.title, difficulty: task.difficulty, run, turns: [], checks: [], exports: [], transcripts: {}, assetNotes: [] };

  const assetIds: Record<string, string> = {};
  const assetByName: Record<string, Asset> = {};
  const setupItems: Record<string, string> = {};
  const analysisStart = Date.now();
  try {
    // import through the real asset pipeline, one file at a time so ids map to names
    for (const name of task.fixtures) {
      const [asset] = await importFiles([join(fixturesDir, name)]);
      if (!asset) throw new Error(`import produced no asset for ${name}`);
      assetIds[name] = asset.id;
    }
    await waitForAssets(Object.values(assetIds), 10 * 60_000);
    const analyzed = getAssets(projectService.projectId);
    for (const [name, id] of Object.entries(assetIds)) {
      const a = analyzed.find((x) => x.id === id)!;
      assetByName[name] = a;
      if (a.status === 'failed') throw new Error(`analysis failed for ${name}: ${a.error}`);
      if (a.error) result.assetNotes!.push(`${name}: ${a.error}`);
    }
    const transcripts = projectTranscripts(projectService.projectId);
    for (const [name, id] of Object.entries(assetIds)) {
      const t = transcripts.find((x) => x.assetId === id);
      if (t) result.transcripts![name] = t.words.map((w) => w.w).join(' ');
    }
    for (const name of task.fixtures) {
      const truth = manifest.fixtures[name];
      if (truth.kind === 'speech' && !result.transcripts![name]) throw new Error(`no transcript produced for ${name} (is the whisper model installed?)`);
    }
  } catch (err) {
    result.infraError = err instanceof Error ? err.message : String(err);
  }
  result.analysisMs = Date.now() - analysisStart;

  const ctx: SetupContext = {
    manifest,
    fps,
    assets: assetByName,
    async addToTimeline(fixture: FixtureName, opts) {
      const asset = assetByName[fixture];
      if (!asset) throw new Error(`fixture ${fixture} was not imported`);
      const before = new Set(projectService.doc.items.map((i) => i.id));
      const args = { assetId: asset.id, ...(opts?.startSec !== undefined ? { startFrame: Math.round(opts.startSec * fps) } : {}) };
      await callTool(asset.kind === 'audio' ? 'addAudio' : 'addClip', args, 'user');
      const added = projectService.doc.items.find((i) => !before.has(i.id));
      if (!added) throw new Error(`placing ${fixture} added no item`);
      setupItems[fixture] = added.id;
      return added.id;
    },
    select: (ids) => editorContextCache.set({ selection: ids }),
    setPlayheadSec: (sec) => editorContextCache.set({ playheadFrame: Math.round(sec * fps) }),
    doc: () => projectService.doc,
  };

  let docs: TimelineDoc[] = [];
  let initialDoc = structuredClone(projectService.doc) as TimelineDoc;
  const turns: TurnTrace[] = [];
  let timedOut = false;
  if (!result.infraError) {
    try {
      await task.setup?.(ctx);
      initialDoc = structuredClone(projectService.doc) as TimelineDoc;
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), task.timeoutMs ?? cfg.taskTimeoutMs);
      let history: HistoryTurn[] = [];
      try {
        for (const [i, message] of task.messages.entries()) {
          const trace = await driver.runTurn(message, history, { chatId: `eval-${task.id}-${run}-${i}`, signal: controller.signal, provider: cfg.provider, taskId: task.id, fps, manifest, assetIds, mode: cfg.mode ?? 'auto' });
          turns.push(trace);
          docs.push(structuredClone(projectService.doc) as TimelineDoc);
          history = historyAfter(history, trace);
          if (trace.timedOut) {
            timedOut = true;
            break;
          }
        }
      } finally {
        clearTimeout(timer);
      }
    } catch (err) {
      result.infraError = `setup/run failed: ${err instanceof Error ? err.message : String(err)}`;
    }
  }
  // a task that stopped early still gets one doc per turn index so checks can index safely
  while (docs.length < task.messages.length) docs.push(structuredClone(projectService.doc) as TimelineDoc);

  const finalDoc = structuredClone(projectService.doc) as TimelineDoc;
  result.exports = await collectExports(projectService.projectId).catch(() => []);
  result.finalItems = compactItems(finalDoc);
  result.project = { width: finalDoc.project.width, height: finalDoc.project.height, fps: finalDoc.project.fps };
  result.turns = turns;
  result.timedOut = timedOut;
  result.stats = traceStats(turns);

  const input: CheckInput = {
    manifest,
    fps,
    assetIds,
    initialDoc,
    docs,
    doc: finalDoc,
    assets: getAssets(projectService.projectId),
    transcripts: projectTranscripts(projectService.projectId),
    turns,
    exports: result.exports,
    setupItems,
  };
  try {
    const scored = result.infraError ? { score: 0, checks: [] } : task.check(input);
    result.score = scored.score;
    result.checks = scored.checks;
  } catch (err) {
    result.score = 0;
    result.infraError = `check crashed: ${err instanceof Error ? err.message : String(err)}`;
  }
  projectService.close();
  return result as TaskResult;
}

function writeResults(cfg: RunConfig, results: TaskResult[], startedAt: string, finished: boolean): void {
  const mean = results.length ? results.reduce((a, r) => a + r.score, 0) / results.length : 0;
  writeFileSync(
    cfg.resultsPath,
    JSON.stringify({ label: cfg.label, driver: cfg.driver, model: cfg.model, provider: cfg.provider, maxSteps: MAX_AGENT_STEPS, startedAt, finished, meanScore: Math.round(mean * 1000) / 1000, results }, null, 2),
  );
}

/** Entry point: returns the process exit code (0 = suite ran to the end, 1 = harness failure). */
export async function runEvals(configPath: string): Promise<number> {
  const { readFileSync } = await import('node:fs');
  const cfg = JSON.parse(readFileSync(configPath, 'utf8')) as RunConfig;
  const driver = DRIVERS[cfg.driver];
  if (!driver) throw new Error(`unknown driver ${cfg.driver}; known: ${Object.keys(DRIVERS).join(', ')}`);
  const manifest = loadManifest(join(cfg.evalsDir, 'fixtures', 'manifest.json'));
  mkdirSync(join(cfg.resultsPath, '..'), { recursive: true });

  // settings of the isolated profile only (userData is the throwaway --user-data-dir)
  await saveSettings({ ai: { agentProvider: cfg.provider as 'llamacpp', agentModel: cfg.model, llamacppUrl: cfg.llamacppUrl, vlmProvider: 'none' }, asr: { model: 'base.en' } });

  const tasks = selectTasks(cfg.tasks);
  const startedAt = new Date().toISOString();
  const results: TaskResult[] = [];
  for (let run = 1; run <= cfg.runs; run++) {
    for (const task of tasks) {
      let attempts = 0;
      let result: TaskResult;
      for (;;) {
        attempts++;
        if (!driver.offline) await waitForServer(cfg.llamacppUrl);
        log(`${task.id} (run ${run}, attempt ${attempts})`);
        result = await runTask(task, run, cfg, driver, manifest);
        // a dead server is not a model failure: wait for it and run the task again
        const dropped = result.turns.some((t) => t.error && CONNECTION_ERROR.test(t.error));
        if (!dropped || attempts >= 4) break;
        log(`${task.id}: connection error, retrying once the server is healthy`);
      }
      result.attempts = attempts;
      results.push(result);
      log(`${task.id}: score ${result.score} in ${Math.round((result.stats?.wallMs ?? 0) / 1000)}s${result.infraError ? ` INFRA: ${result.infraError}` : ''}`);
      writeResults(cfg, results, startedAt, false);
    }
  }
  writeResults(cfg, results, startedAt, true);
  return results.some((r) => r.infraError) ? 1 : 0;
}
