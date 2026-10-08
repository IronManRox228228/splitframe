/**
 * Seconds and timecodes for the model-facing layer. The model talks in seconds ("7.5", "0:07",
 * "1m30s"); code converts to frames with the project fps. Pure, no Electron imports.
 */

export const toFrames = (sec: number, fps: number): number => Math.round(sec * fps);
export const toSec = (frames: number, fps: number): number => frames / fps;

/** Seconds from a number or a loose timecode string ("7.5", "7.5s", "0:07", "1:02.5", "1m30s", "90 sec"); null if unparseable. */
export function parseTime(value: unknown): number | null {
  if (typeof value === 'number') return Number.isFinite(value) && value >= 0 ? value : null;
  if (typeof value !== 'string') return null;
  const s = value.trim().toLowerCase();
  if (!s) return null;
  const clock = /^(?:(\d+):)?(\d{1,2}):(\d{1,2}(?:\.\d+)?)$/.exec(s);
  if (clock) return Number(clock[1] ?? 0) * 3600 + Number(clock[2]) * 60 + Number(clock[3]);
  const units = /^(?:(\d+(?:\.\d+)?)\s*(?:h|hr|hrs|hours?))?\s*(?:(\d+(?:\.\d+)?)\s*(?:m|min|mins|minutes?))?\s*(?:(\d+(?:\.\d+)?)\s*(?:s|sec|secs|seconds?)?)?$/.exec(s);
  if (units && (units[1] || units[2] || units[3])) return Number(units[1] ?? 0) * 3600 + Number(units[2] ?? 0) * 60 + Number(units[3] ?? 0);
  return null;
}

/** "0:07", "0:07.5", "1:02:03": tenths only when the time is not a whole second. */
export function fmtTime(sec: number): string {
  const tenths = Math.round(Math.max(0, sec) * 10);
  const whole = tenths % 10 === 0;
  const total = Math.floor(tenths / 10);
  const s = total % 60;
  const m = Math.floor(total / 60) % 60;
  const h = Math.floor(total / 3600);
  const pad = (n: number) => String(n).padStart(2, '0');
  const frac = whole ? '' : `.${tenths % 10}`;
  return h > 0 ? `${h}:${pad(m)}:${pad(s)}${frac}` : `${m}:${pad(s)}${frac}`;
}

/** "5s", "5.4s", "1m12s" */
export function fmtDur(sec: number): string {
  const rounded = Math.round(Math.max(0, sec) * 10) / 10;
  if (rounded < 60) return `${Number.isInteger(rounded) ? rounded : rounded.toFixed(1)}s`;
  const m = Math.floor(rounded / 60);
  const s = Math.round(rounded - m * 60);
  return s === 60 ? `${m + 1}m` : s ? `${m}m${s}s` : `${m}m`;
}

/** "9:16", "16:9" (reduced by gcd for odd sizes, e.g. 1080x1350 -> 4:5) */
export function aspectLabel(width: number, height: number): string {
  const gcd = (a: number, b: number): number => (b ? gcd(b, a % b) : a);
  const g = gcd(width, height) || 1;
  const a = width / g;
  const b = height / g;
  return a > 40 || b > 40 ? `${(width / height).toFixed(2)}:1` : `${a}:${b}`;
}
