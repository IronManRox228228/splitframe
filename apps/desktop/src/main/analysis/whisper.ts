import { spawn, spawnSync } from 'node:child_process';
import { accessSync, constants, existsSync, readFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { app } from 'electron';
import { getPaths } from '../paths.ts';
import { runFfmpeg } from '../ffmpeg.ts';

/**
 * ASR via whisper.cpp (addendum §3): word-level timestamps. Prefers a bundled whisper-cli
 * in bin/<plat>-<arch>/, then the PATH (dev: brew's whisper-cpp). Models live in
 * userData/models and are fetched by the model manager / scripts/fetch-models.
 */

export const WHISPER_MODELS = [
  { id: 'tiny.en', sizeMB: 78, note: 'fastest, English' },
  { id: 'base.en', sizeMB: 148, note: 'balanced, English (default)' },
  { id: 'small.en', sizeMB: 488, note: 'better accuracy, English' },
  { id: 'large-v3', sizeMB: 3100, note: 'best, multilingual, needs 16GB+' },
] as const;

export type WhisperModelId = (typeof WHISPER_MODELS)[number]['id'];

export interface WhisperWord {
  w: string;
  startMs: number;
  endMs: number;
  conf: number;
}

interface WhisperToken {
  text: string;
  offsets: { from: number; to: number };
  p?: number;
  id?: number;
}

function isExecutable(p: string): boolean {
  try {
    accessSync(p, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

export function locateWhisperCli(): string | null {
  const { bundledBinDir } = getPaths();
  const name = process.platform === 'win32' ? 'whisper-cli.exe' : 'whisper-cli';
  const bundled = join(bundledBinDir, name);
  if (isExecutable(bundled)) return bundled;
  const which = spawnSync(process.platform === 'win32' ? 'where' : 'which', [name], { encoding: 'utf8' });
  const first = which.status === 0 ? which.stdout.split('\n')[0]?.trim() : null;
  if (first && isExecutable(first)) return first;
  // brew / common install locations
  for (const candidate of ['/opt/homebrew/bin/whisper-cli', '/usr/local/bin/whisper-cli']) {
    if (isExecutable(candidate)) return candidate;
  }
  return null;
}

export function modelPath(modelId: string): string {
  return join(app.getPath('userData'), 'models', `ggml-${modelId}.bin`);
}

export function hasModel(modelId: string): boolean {
  return existsSync(modelPath(modelId));
}

/** Special-token guard: whisper.cpp emits timestamp/special tokens like [_TT_50_] / [SOT]. */
function isSpecialToken(text: string): boolean {
  return text.startsWith('[') || text.startsWith('<|') || text.startsWith('_TT_');
}

/** Decode a 16kHz mono wav, run whisper-cli with full JSON output, parse words. */
export async function transcribe(mediaPath: string, opts: TranscribeOptions): Promise<{ words: WhisperWord[]; language: string }> {
  const stamp = Date.now();
  try {
    return await transcribeFiles(mediaPath, opts, stamp);
  } finally {
    // the 16 kHz wav and whisper's json are scratch files; don't leave them in the project cache
    for (const ext of ['wav', 'json']) {
      try {
        unlinkSync(join(opts.workDir, `asr-${stamp}.${ext}`));
      } catch {
        /* already gone */
      }
    }
  }
}

type TranscribeOptions = {
  modelId: string;
  workDir: string;
  language?: string;
  signal?: AbortSignal;
  progress?: (p: number) => void;
};

async function transcribeFiles(
  mediaPath: string,
  opts: TranscribeOptions,
  stamp: number,
): Promise<{ words: WhisperWord[]; language: string }> {
  const cli = locateWhisperCli();
  if (!cli) throw new Error('whisper-cli not found. Install it (brew install whisper-cpp) or run pnpm fetch:whisper.');
  const model = modelPath(opts.modelId);
  if (!existsSync(model)) throw new Error(`Whisper model ${opts.modelId} not downloaded yet.`);

  const wavPath = join(opts.workDir, `asr-${stamp}.wav`);
  await new Promise<void>((resolve, reject) => {
    runFfmpeg(['-i', mediaPath, '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', '-vn', wavPath])
      .then((r) => (r.code === 0 ? resolve() : reject(new Error(`audio decode failed: ${r.stderr.split('\n').slice(-2).join(' ')}`))))
      .catch(reject);
  });

  const outBase = join(opts.workDir, `asr-${stamp}`);
  const args = [
    '-m', model,
    '-f', wavPath,
    '-ojf', '-of', outBase,
    '--max-len', '1', // forces token-level timestamps suitable for word timing
    '-ml', '1',
    '-sow',
  ];
  if (opts.language) args.push('-l', opts.language);

  const jsonPath = `${outBase}.json`;
  let detectedLanguage: string | undefined;
  const words = await new Promise<WhisperWord[]>((resolve, reject) => {
    const child = spawn(cli, args, { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
    let stderr = '';
    child.stderr.on('data', (d) => {
      const text = String(d);
      stderr += text;
      // whisper-cli prints progress lines like "decode time" but also a percent-ish
      // progress via \r on some builds; approximate by counting processed segments later.
      opts.progress?.(0.5);
    });
    opts.signal?.addEventListener('abort', () => child.kill('SIGKILL'));
    child.on('error', reject);
    child.on('close', (code) => {
      if (code !== 0 && opts.signal?.aborted) return reject(new Error('aborted'));
      if (code !== 0) return reject(new Error(`whisper-cli exited ${code}: ${stderr.split('\n').slice(-3).join(' | ')}`));
      try {
        const data = JSON.parse(readFileSync(jsonPath, 'utf8')) as {
          transcription?: { offsets: { from: number; to: number }; tokens?: WhisperToken[] }[];
          result?: { language?: string };
        };
        detectedLanguage = data.result?.language;
        const words: WhisperWord[] = [];
        for (const segment of data.transcription ?? []) {
          const tokens = (segment.tokens ?? []).filter((t) => !isSpecialToken(t.text));
          let current: WhisperWord | null = null;
          for (const token of tokens) {
            const text = token.text.replace(/▁/g, ' ');
            const startsWord = text.startsWith(' ') || current === null;
            const clean = text.trim();
            if (!clean) continue;
            if (startsWord && current) words.push(current);
            if (startsWord) {
              current = {
                w: clean,
                startMs: token.offsets.from,
                endMs: token.offsets.to,
                conf: token.p ?? 1,
              };
            } else if (current) {
              current.w += clean;
              current.endMs = token.offsets.to;
            }
          }
          if (current) words.push(current);
        }
        resolve(words);
      } catch (err) {
        reject(new Error(`failed to parse whisper output: ${err instanceof Error ? err.message : String(err)}`));
      }
    });
  });

  const language = opts.language ?? detectedLanguage ?? 'en';
  return { words, language };
}
