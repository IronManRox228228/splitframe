import { runFfmpeg } from '../ffmpeg.ts';
import { parseSilenceIntervals } from './footage-notes-parse.ts';

/**
 * Silence map of an asset's audio: the actual quiet intervals, measured from the waveform.
 * Whisper stretches word end times across pauses, so transcript gaps cannot reveal them;
 * the cut tools (removeSilences, buildRoughCut, duckMusic) read this instead.
 */

export interface SilenceInterval {
  startMs: number;
  endMs: number;
}

/** Bump to recompute cached maps when the detector settings change. */
export const SILENCE_MAP_VERSION = 1;
export const SILENCE_NOISE_DB = -40;
/** shortest quiet stretch recorded; the tools filter by their own threshold */
export const SILENCE_MIN_SEC = 0.2;

export function silenceFilter(): string {
  return `silencedetect=n=${SILENCE_NOISE_DB}dB:d=${SILENCE_MIN_SEC}`;
}

export function silenceIntervalsFromStderr(stderr: string, durationMs: number): SilenceInterval[] {
  return parseSilenceIntervals(stderr, durationMs / 1000).map((r) => ({
    startMs: Math.round(r.startSec * 1000),
    endMs: Math.round(r.endSec * 1000),
  }));
}

export async function computeSilenceMap(mediaPath: string, durationMs: number, signal?: AbortSignal): Promise<SilenceInterval[]> {
  const r = await runFfmpeg(['-i', mediaPath, '-vn', '-af', silenceFilter(), '-f', 'null', '-'], { signal });
  if (r.code !== 0) throw new Error(`silence detection failed: ${r.stderr.trim().split('\n').slice(-2).join(' | ')}`);
  return silenceIntervalsFromStderr(r.stderr, durationMs);
}
