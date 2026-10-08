import { spawn, spawnSync } from 'node:child_process';
import { accessSync, constants } from 'node:fs';
import { join } from 'node:path';
import { getPaths } from './paths.ts';
import { binaryFileName, chooseH264Encoder } from './ffmpeg-utils.ts';

/**
 * ffmpeg/ffprobe wrapper. Prefers the bundled LGPL build in bin/<plat>-<arch>/ and falls
 * back to the PATH copy for development. The encoder choice prefers OS hardware encoders
 * so the bundled LGPL build never needs GPL libx264 (docs/DECISIONS.md #5).
 */

export interface FfmpegInfo {
  ffmpegPath: string;
  ffprobePath: string;
  source: 'bundled' | 'path';
  h264Encoder: string;
  version: string;
}

let cached: FfmpegInfo | null = null;

function isExecutable(p: string): boolean {
  try {
    accessSync(p, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function locateBinary(name: string): { path: string; source: 'bundled' | 'path' } | null {
  const { bundledBinDir } = getPaths();
  // bundled binaries carry the platform suffix (ffmpeg.exe on Windows)
  const fileName = binaryFileName(name);
  const bundled = join(bundledBinDir, fileName);
  if (isExecutable(bundled)) return { path: bundled, source: 'bundled' };
  // dev fallback: PATH lookup
  const which = spawnSync(process.platform === 'win32' ? 'where' : 'which', [fileName], {
    encoding: 'utf8',
  });
  const first = which.status === 0 ? which.stdout.split('\n')[0]?.trim() : null;
  if (first && isExecutable(first)) return { path: first, source: 'path' };
  return null;
}

/** Trial-encode one frame: `-encoders` also lists hardware encoders the machine can't run. */
function encoderWorks(ffmpegPath: string, encoder: string): boolean {
  try {
    const res = spawnSync(
      ffmpegPath,
      [
        '-hide_banner', '-v', 'error',
        '-f', 'lavfi', '-i', 'color=c=black:s=256x256:d=0.2',
        '-frames:v', '1', '-c:v', encoder, '-pix_fmt', 'yuv420p',
        '-f', 'null', '-',
      ],
      { encoding: 'utf8', timeout: 15000, windowsHide: true },
    );
    return res.status === 0;
  } catch {
    return false;
  }
}

function pickH264Encoder(ffmpegPath: string): string {
  try {
    const res = spawnSync(ffmpegPath, ['-hide_banner', '-encoders'], { encoding: 'utf8', windowsHide: true });
    return chooseH264Encoder(res.stdout ?? '', process.platform, (enc) => encoderWorks(ffmpegPath, enc));
  } catch {
    return 'mpeg4';
  }
}

export function getFfmpeg(): FfmpegInfo {
  if (cached) return cached;
  const ffmpeg = locateBinary('ffmpeg');
  const ffprobe = locateBinary('ffprobe');
  if (!ffmpeg || !ffprobe) {
    throw new Error(
      'ffmpeg/ffprobe not found. Run `pnpm fetch:ffmpeg` to download bundled binaries, or install ffmpeg on your PATH.',
    );
  }
  const versionOut = spawnSync(ffmpeg.path, ['-version'], { encoding: 'utf8' }).stdout ?? '';
  cached = {
    ffmpegPath: ffmpeg.path,
    ffprobePath: ffprobe.path,
    source: ffmpeg.source,
    h264Encoder: pickH264Encoder(ffmpeg.path),
    version: (versionOut.split('\n')[0] ?? 'ffmpeg').replace('ffmpeg version ', '').split(' ')[0] ?? '',
  };
  return cached;
}

export interface RunResult {
  code: number;
  stdout: string;
  stderr: string;
}

export function runFfmpeg(
  args: string[],
  opts: { onStderr?: (line: string) => void; onStdin?: (stdin: NodeJS.WritableStream) => void; signal?: AbortSignal } = {},
): Promise<RunResult> {
  const { ffmpegPath } = getFfmpeg();
  return new Promise((resolve, reject) => {
    const child = spawn(ffmpegPath, ['-hide_banner', '-y', ...args], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += String(d)));
    child.stderr.on('data', (d) => {
      const text = String(d);
      stderr += text;
      opts.onStderr?.(text);
    });
    if (opts.onStdin) opts.onStdin(child.stdin);
    else child.stdin.end();
    if (opts.signal?.aborted) child.kill('SIGKILL');
    else opts.signal?.addEventListener('abort', () => child.kill('SIGKILL'));
    child.on('error', reject);
    child.on('close', (code) => resolve({ code: code ?? -1, stdout, stderr }));
  });
}

/** Like runFfmpeg, but a non-zero exit (other than a deliberate abort) rejects with ffmpeg's last stderr lines. */
export async function runFfmpegChecked(args: string[], opts: Parameters<typeof runFfmpeg>[1] = {}): Promise<RunResult> {
  const result = await runFfmpeg(args, opts);
  if (result.code !== 0 && !opts.signal?.aborted) {
    throw new Error(`ffmpeg exited with ${result.code}: ${result.stderr.trim().split('\n').slice(-3).join(' | ')}`);
  }
  return result;
}

/**
 * Hardware decode for long decode jobs (proxies, scene detection): about twice as fast
 * as software on 4K HEVC. CUDA when NVENC works, so decode and encode share the NVIDIA
 * GPU (d3d11va would decode on the iGPU and NVENC then fails); otherwise ffmpeg's pick.
 */
export function hwDecodeArgs(): string[] {
  return ['-hwaccel', getFfmpeg().h264Encoder === 'h264_nvenc' ? 'cuda' : 'auto'];
}

/**
 * Runs `build(hwDecodeArgs())`, and if that fails for any reason other than an abort
 * (driver, codec or adapter trouble), runs `build([])` in software instead.
 */
export async function runWithHwDecode(
  build: (hw: string[]) => string[],
  opts: Parameters<typeof runFfmpeg>[1] = {},
): Promise<RunResult> {
  const result = await runFfmpeg(build(hwDecodeArgs()), opts);
  if (result.code === 0 || opts.signal?.aborted) return result;
  return runFfmpeg(build([]), opts);
}

export interface ProbeResult {
  durationMs: number;
  width: number;
  height: number;
  fps: number | undefined;
  hasVideo: boolean;
  hasAudio: boolean;
  codec: string | undefined;
  rotation?: number;
}

export async function ffprobe(file: string): Promise<ProbeResult> {
  const { ffprobePath } = getFfmpeg();
  const { execFile } = await import('node:child_process');
  const { promisify } = await import('node:util');
  const run = promisify(execFile);
  const { stdout } = await run(
    ffprobePath,
    ['-print_format', 'json', '-show_format', '-show_streams', '-loglevel', 'quiet', file],
    { maxBuffer: 10 * 1024 * 1024 },
  );
  const data = JSON.parse(stdout) as {
    streams: {
      codec_type: string;
      codec_name?: string;
      width?: number;
      height?: number;
      r_frame_rate?: string;
      duration?: string;
      side_data_list?: { rotation?: number }[];
      tags?: { rotate?: string };
    }[];
    format: { duration?: string };
  };
  const video = data.streams.find((s) => s.codec_type === 'video');
  const audio = data.streams.find((s) => s.codec_type === 'audio');
  const rate = video?.r_frame_rate ?? '0/1';
  const [numStr, denStr] = rate.split('/');
  const num = Number(numStr);
  const den = Number(denStr);
  const fps = num > 0 && den > 0 ? num / den : undefined;
  const durationSec = Number(data.format?.duration ?? video?.duration ?? audio?.duration ?? 0);
  const rotation =
    video?.side_data_list?.find((d) => typeof d.rotation === 'number')?.rotation ??
    (video?.tags?.rotate ? Number(video.tags.rotate) : undefined);
  return {
    durationMs: Math.round(durationSec * 1000),
    width: video?.width ?? 0,
    height: video?.height ?? 0,
    fps: fps && Number.isFinite(fps) ? fps : undefined,
    hasVideo: Boolean(video),
    hasAudio: Boolean(audio),
    codec: video?.codec_name,
    rotation,
  };
}

export async function makeProxy(
  src: string,
  dest: string,
  opts: { height?: number; onProgress?: (t: number, d: number) => void; signal?: AbortSignal; durationMs?: number } = {},
): Promise<void> {
  const { h264Encoder } = getFfmpeg();
  const height = opts.height ?? 540;
  const encArgs: Record<string, string[]> = {
    h264_videotoolbox: ['-b:v', '1500k'],
    h264_nvenc: ['-b:v', '1500k'],
    h264_qsv: ['-b:v', '1500k'],
    h264_amf: ['-b:v', '1500k'],
    libopenh264: ['-b:v', '1500k'],
    libx264: ['-crf', '28', '-preset', 'veryfast'],
    mpeg4: ['-b:v', '1500k'],
  };
  const args = (hw: string[]) => [
    ...hw,
    '-i', src,
    '-vf', `scale=-2:${height}`,
    '-c:v', h264Encoder,
    ...(encArgs[h264Encoder] ?? []),
    '-pix_fmt', 'yuv420p',
    '-c:a', 'aac', '-b:a', '128k', '-ac', '2',
    '-movflags', '+faststart',
    '-progress', 'pipe:2', '-nostats',
    dest,
  ];
  const result = await runWithHwDecode(args, {
    signal: opts.signal,
    onStderr: (text) => {
      const m = /out_time_ms=(\d+)/.exec(text);
      if (m && opts.durationMs && opts.onProgress) {
        opts.onProgress(Number(m[1]) / 1000, opts.durationMs);
      }
    },
  });
  if (result.code !== 0 && !opts.signal?.aborted) {
    throw new Error(`ffmpeg exited with ${result.code}: ${result.stderr.trim().split('\n').slice(-3).join(' | ')}`);
  }
}

export async function makeThumbnail(src: string, dest: string, atSec = 1): Promise<void> {
  await runFfmpegChecked([
    '-ss', String(atSec),
    '-i', src,
    '-frames:v', '1',
    '-vf', 'scale=320:-2',
    dest,
  ]);
}

export async function makeWaveform(src: string, dest: string): Promise<void> {
  await runFfmpegChecked([
    '-i', src,
    '-filter_complex', 'showwavespic=s=800x120:colors=#5FB7A1:split_channels=0',
    '-frames:v', '1',
    dest,
  ]);
}

/** Loudness-normalized wav extraction (used by export audio mixing). */
export async function extractAudio(src: string, dest: string, signal?: AbortSignal): Promise<void> {
  await runFfmpegChecked(['-i', src, '-vn', '-c:a', 'pcm_f32le', dest], { signal });
}
