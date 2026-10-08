import { join } from 'node:path';
import { runFfmpeg, runWithHwDecode } from '../ffmpeg.ts';

/**
 * Scene detection (main prompt §5): ffmpeg's scene-change selection. Returns shot
 * boundaries in ms; 1–2 keyframes per scene are extracted by the caller.
 */
export interface SceneBoundary {
  startMs: number;
  endMs: number;
  score: number;
}

export async function detectScenes(
  mediaPath: string,
  durationMs: number,
  opts: { threshold?: number; signal?: AbortSignal } = {},
): Promise<SceneBoundary[]> {
  const threshold = opts.threshold ?? 0.25;
  const result = await runWithHwDecode((hw) => [
    ...hw,
    '-i', mediaPath,
    '-vf', `select='gt(scene,${threshold})',showinfo`,
    '-an',
    '-f', 'null',
    '-',
  ], { signal: opts.signal });

  // showinfo lines: "[Parsed_showinfo...] n:   0 pts_time:1.234 ..." (from the run that counted,
  // not a failed hardware attempt that was retried in software)
  const times: number[] = [0];
  for (const match of result.stderr.matchAll(/pts_time:(\d+(?:\.\d+)?)/g)) {
    times.push(Number(match[1]) * 1000);
  }
  if (durationMs > 0) times.push(durationMs);

  const boundaries: SceneBoundary[] = [];
  for (let i = 0; i < times.length - 1; i++) {
    const startMs = Math.round(times[i]!);
    const endMs = Math.round(times[i + 1]!);
    if (endMs - startMs < 250) continue; // ignore sub-frame flaps
    boundaries.push({ startMs, endMs, score: 1 });
  }
  return boundaries;
}

export async function extractKeyframe(
  mediaPath: string,
  atMs: number,
  dest: string,
): Promise<void> {
  await runFfmpeg(['-ss', (atMs / 1000).toFixed(3), '-i', mediaPath, '-frames:v', '1', '-vf', 'scale=512:-2', dest]);
}

export function keyframePath(cacheDir: string, assetId: string, index: number): string {
  return join(cacheDir, `${assetId}-scene-${index}.jpg`);
}
