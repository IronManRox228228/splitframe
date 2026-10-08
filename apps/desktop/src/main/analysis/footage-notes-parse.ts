/**
 * Pure parsers for the ffmpeg filter output used by footage notes. Kept free of any
 * process or electron imports so they can be unit-tested on captured output.
 *
 * Video pass (one decode, sampled at a fixed rate and scaled down):
 *   fps=N,scale=W:-2,signalstats,blurdetect,siti,blackdetect,freezedetect,metadata=mode=print:file=-
 * Per-frame values arrive on stdout as `frame:` headers followed by `lavfi.*=value` lines;
 * black/freeze intervals arrive on stderr as filter log lines.
 * Audio pass: ebur128=metadata=1, ametadata=mode=print:key=lavfi.r128.M:file=-, silencedetect.
 */

export interface VideoSample {
  /** seconds from the start of the media */
  t: number;
  yAvg?: number;
  yLow?: number;
  yHigh?: number;
  blur?: number;
  /** spatial information (edge energy) */
  si?: number;
  /** temporal information (frame-to-frame change) */
  ti?: number;
}

const VIDEO_KEYS: Record<string, keyof Omit<VideoSample, 't'>> = {
  'lavfi.signalstats.YAVG': 'yAvg',
  'lavfi.signalstats.YLOW': 'yLow',
  'lavfi.signalstats.YHIGH': 'yHigh',
  'lavfi.blur': 'blur',
  'lavfi.siti.si': 'si',
  'lavfi.siti.ti': 'ti',
};

/** Parses the metadata=print stream: `frame:N pts:P pts_time:T` then `key=value` lines. */
export function parseVideoSamples(stdout: string): VideoSample[] {
  const samples: VideoSample[] = [];
  let current: VideoSample | null = null;
  for (const raw of stdout.split('\n')) {
    const line = raw.trim();
    if (line.startsWith('frame:')) {
      const m = /pts_time:(-?\d+(?:\.\d+)?)/.exec(line);
      if (!m) {
        current = null;
        continue;
      }
      const t = Number(m[1]);
      // several metadata filters may print the same frame; merge by timestamp
      const last = samples[samples.length - 1];
      if (last && last.t === t) current = last;
      else {
        current = { t };
        samples.push(current);
      }
      continue;
    }
    if (!current) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const field = VIDEO_KEYS[line.slice(0, eq)];
    if (!field) continue;
    const value = Number(line.slice(eq + 1));
    if (Number.isFinite(value)) current[field] = value;
  }
  return samples;
}

export interface TimeRange {
  startSec: number;
  endSec: number;
}

/** `[blackdetect @ ..] black_start:3 black_end:5 black_duration:2` */
export function parseBlackIntervals(stderr: string): TimeRange[] {
  const out: TimeRange[] = [];
  for (const m of stderr.matchAll(/black_start:(\d+(?:\.\d+)?)\s+black_end:(\d+(?:\.\d+)?)/g)) {
    out.push({ startSec: Number(m[1]), endSec: Number(m[2]) });
  }
  return out;
}

/**
 * freezedetect logs `freeze_start: S` and later `freeze_end: E`. A freeze that runs to the
 * end of the media may have no end line; it is closed at `totalSec`.
 */
export function parseFreezeIntervals(stderr: string, totalSec: number): TimeRange[] {
  const out: TimeRange[] = [];
  let start: number | null = null;
  for (const m of stderr.matchAll(/freeze_(start|end):\s*(\d+(?:\.\d+)?)/g)) {
    const value = Number(m[2]);
    if (m[1] === 'start') {
      if (start !== null) out.push({ startSec: start, endSec: totalSec });
      start = value;
    } else if (start !== null) {
      out.push({ startSec: start, endSec: value });
      start = null;
    }
  }
  if (start !== null && totalSec > start) out.push({ startSec: start, endSec: totalSec });
  return out;
}

export interface LoudnessSample {
  t: number;
  /** momentary loudness, LUFS (about -120 means digital silence) */
  m: number;
}

/** Output of `ametadata=mode=print:key=lavfi.r128.M:file=-` */
export function parseLoudnessSamples(stdout: string): LoudnessSample[] {
  const out: LoudnessSample[] = [];
  let t: number | null = null;
  for (const raw of stdout.split('\n')) {
    const line = raw.trim();
    if (line.startsWith('frame:')) {
      const m = /pts_time:(-?\d+(?:\.\d+)?)/.exec(line);
      t = m ? Number(m[1]) : null;
    } else if (t !== null && line.startsWith('lavfi.r128.M=')) {
      const v = Number(line.slice('lavfi.r128.M='.length));
      if (Number.isFinite(v)) out.push({ t, m: v });
    }
  }
  return out;
}

/**
 * `silence_start: 5` ... `silence_end: 8.01 | silence_duration: 3.01`. Silence still open at
 * the end of the stream is closed at `totalSec`.
 */
export function parseSilenceIntervals(stderr: string, totalSec: number): TimeRange[] {
  const out: TimeRange[] = [];
  let start: number | null = null;
  for (const m of stderr.matchAll(/silence_(start|end):\s*(-?\d+(?:\.\d+)?)/g)) {
    const value = Math.max(0, Number(m[2]));
    if (m[1] === 'start') {
      start = value;
    } else if (start !== null) {
      out.push({ startSec: start, endSec: value });
      start = null;
    } else {
      out.push({ startSec: 0, endSec: value });
    }
  }
  if (start !== null && totalSec > start) out.push({ startSec: start, endSec: totalSec });
  return out;
}

/** Integrated loudness (LUFS) from the ebur128 summary block, or null when absent. */
export function parseIntegratedLoudness(stderr: string): number | null {
  const m = /Integrated loudness:\s*\r?\n\s*I:\s*(-?\d+(?:\.\d+)?)\s*LUFS/.exec(stderr);
  if (!m) return null;
  const v = Number(m[1]);
  return Number.isFinite(v) && v > -70 ? v : null;
}
