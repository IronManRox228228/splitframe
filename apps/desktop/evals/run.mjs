// Eval launcher:  pnpm --dir apps/desktop eval -- [--tasks id1,id2] [--label baseline] [--runs 1]
//                                                 [--driver chat-v0] [--no-build] [--url http://127.0.0.1:8080]
// Builds the app if the sources changed, creates a throwaway profile + projects folder, copies
// (never downloads) the Whisper model, runs the suite inside Electron's main process and
// prints a summary table. Results: evals/results/<timestamp>-<label>.json and .md
import { spawn, spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const APP = join(HERE, '..');
const ROOT = join(APP, '..', '..');

const args = process.argv.slice(2).filter((a) => a !== '--');
const opt = (name, fallback) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : fallback;
};
const label = opt('label', 'run').replace(/[^a-z0-9._-]+/gi, '-');
const runs = Number(opt('runs', '1'));
const tasks = opt('tasks', '') ? opt('tasks', '').split(',').filter(Boolean) : undefined;
const url = opt('url', 'http://127.0.0.1:8080');
const driver = opt('driver', 'chat-v0');
const model = opt('model', 'qwythos-9b-v2');
const mode = opt('mode', 'auto'); // agent mode for harness-v1: evals run in auto so no card waits for a human

// ---- electron binary (pnpm layout) ----
function findElectron() {
  const store = join(ROOT, 'node_modules', '.pnpm');
  const dir = readdirSync(store).find((d) => d.startsWith('electron@'));
  if (!dir) throw new Error('electron is not installed');
  return join(store, dir, 'node_modules', 'electron', 'dist', process.platform === 'win32' ? 'electron.exe' : 'electron');
}

// ---- fixtures ----
if (!existsSync(join(HERE, 'fixtures', 'manifest.json')) || !existsSync(join(HERE, 'fixtures', 'out', 'interview.mp4'))) {
  console.log('generating fixtures...');
  const r = spawnSync(process.execPath, [join(HERE, 'fixtures', 'generate.mjs')], { stdio: 'inherit' });
  if (r.status !== 0) process.exit(1);
}

// ---- build if any source is newer than the build ----
function newest(dir, skip = new Set(['node_modules', 'out', 'results', 'fixtures'])) {
  let t = 0;
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    if (skip.has(e.name)) continue;
    const p = join(dir, e.name);
    t = Math.max(t, e.isDirectory() ? newest(p, skip) : statSync(p).mtimeMs);
  }
  return t;
}
const builtAt = existsSync(join(APP, 'out', 'main', 'index.js')) ? statSync(join(APP, 'out', 'main', 'index.js')).mtimeMs : 0;
const sourcesAt = Math.max(newest(join(APP, 'src')), newest(HERE, new Set(['results', 'fixtures'])), ...['tools', 'schema', 'editor-core', 'agent', 'renderer', 'platform'].map((p) => newest(join(ROOT, 'packages', p, 'src'))));
if (!args.includes('--no-build') && sourcesAt > builtAt) {
  console.log('building app (sources changed)...');
  const r = spawnSync('pnpm', ['exec', 'electron-vite', 'build'], { cwd: APP, stdio: 'inherit', shell: true });
  if (r.status !== 0) process.exit(1);
}

// ---- isolated profile ----
const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
const work = join(tmpdir(), `splitframe-eval-${stamp}`);
const profile = join(work, 'profile');
const projects = join(work, 'projects');
mkdirSync(join(profile, 'models'), { recursive: true });
mkdirSync(projects, { recursive: true });
// read-only copy of the installed Whisper model(s); nothing is downloaded
const userModels = join(process.env.APPDATA ?? '', 'Cutboard', 'models');
if (existsSync(userModels)) {
  for (const f of readdirSync(userModels)) {
    if (/^ggml-.*\.bin$/.test(f)) copyFileSync(join(userModels, f), join(profile, 'models', f));
  }
}
if (!existsSync(join(profile, 'models', 'ggml-base.en.bin'))) console.warn('warning: no ggml-base.en.bin found, speech tasks will fail at analysis');

const resultsDir = join(HERE, 'results');
mkdirSync(resultsDir, { recursive: true });
const resultsPath = join(resultsDir, `${stamp}-${label}.json`);
const configPath = join(work, 'run-config.json');
writeFileSync(
  configPath,
  JSON.stringify({ label, driver, mode, tasks, runs, provider: 'llamacpp', model, llamacppUrl: url, evalsDir: HERE, resultsPath, taskTimeoutMs: 10 * 60_000 }, null, 2),
);

console.log(`profile ${profile}\nresults ${resultsPath}`);
const child = spawn(findElectron(), [APP, `--user-data-dir=${profile}`], {
  cwd: APP,
  env: { ...process.env, CUTBOARD_EVAL: configPath, CUTBOARD_PROJECTS_ROOT: projects },
  stdio: ['ignore', 'inherit', 'inherit'],
});
child.on('exit', (code) => {
  if (!existsSync(resultsPath)) {
    console.error(`no results written (electron exit code ${code})`);
    process.exit(1);
  }
  const data = JSON.parse(readFileSync(resultsPath, 'utf8'));
  const md = summaryMarkdown(data);
  writeFileSync(resultsPath.replace(/\.json$/, '.md'), md);
  console.log(`\n${table(data)}\n\nmean score ${data.meanScore}  (exit ${code})\nwork dir (disposable): ${work}`);
  process.exit(code ?? 1);
});

// ---- reporting ----
function rows(data) {
  return data.results.map((r) => ({
    task: r.id,
    score: r.score.toFixed(2),
    pass: r.score >= 0.999 ? 'PASS' : r.infraError ? 'INFRA' : r.score >= 0.5 ? 'part' : 'FAIL',
    calls: r.stats?.toolCalls ?? 0,
    invalid: r.stats?.invalidCalls ?? 0,
    failed: r.stats?.failedCalls ?? 0,
    steps: r.stats?.steps ?? 0,
    cap: r.stats?.capHit ? 'yes' : 'no',
    wall: `${Math.round((r.stats?.wallMs ?? 0) / 1000)}s`,
  }));
}
function table(data) {
  const rs = rows(data);
  const cols = ['task', 'score', 'pass', 'calls', 'invalid', 'failed', 'steps', 'cap', 'wall'];
  const w = cols.map((c) => Math.max(c.length, ...rs.map((r) => String(r[c]).length)));
  const line = (r) => cols.map((c, i) => String(r[c]).padEnd(w[i])).join('  ');
  return [line(Object.fromEntries(cols.map((c) => [c, c]))), ...rs.map(line)].join('\n');
}
function summaryMarkdown(data) {
  const rs = rows(data);
  const head = `# Eval ${data.label} (${data.driver}, ${data.model})\n\nStarted ${data.startedAt}. Mean score **${data.meanScore}** over ${rs.length} task run(s). Step cap ${data.maxSteps}.\n\n`;
  const tbl = ['| task | score | result | tool calls | invalid | failed | steps | cap hit | wall |', '|---|---|---|---|---|---|---|---|---|', ...rs.map((r) => `| ${r.task} | ${r.score} | ${r.pass} | ${r.calls} | ${r.invalid} | ${r.failed} | ${r.steps} | ${r.cap} | ${r.wall} |`)].join('\n');
  const detail = data.results
    .map((r) => `\n### ${r.id}\n${r.checks.map((c) => `- [${c.pass ? 'x' : c.partial > 0 ? '~' : ' '}] ${c.name}: ${c.detail}`).join('\n')}${r.infraError ? `\n- INFRA: ${r.infraError}` : ''}`)
    .join('\n');
  return `${head}${tbl}\n${detail}\n`;
}
