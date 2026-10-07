/** Small display helpers for the project grid and export dialog. */

export function formatDuration(ms: number): string {
  const total = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const ss = String(s).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
}

const KNOWN_RATIOS: [string, number][] = [
  ['9:16', 9 / 16],
  ['16:9', 16 / 9],
  ['1:1', 1],
  ['4:5', 4 / 5],
  ['5:4', 5 / 4],
  ['3:4', 3 / 4],
  ['4:3', 4 / 3],
  ['21:9', 21 / 9],
];

export function aspectLabel(width: number, height: number): string {
  if (width <= 0 || height <= 0) return '16:9';
  const r = width / height;
  const hit = KNOWN_RATIOS.find(([, v]) => Math.abs(v - r) / v < 0.02);
  if (hit) return hit[0];
  const g = (a: number, b: number): number => (b === 0 ? a : g(b, a % b));
  const d = g(width, height);
  const [w, h] = [width / d, height / d];
  return w <= 32 && h <= 32 ? `${w}:${h}` : `${r.toFixed(2)}:1`;
}

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

/** "Edited 12 min ago", "Edited 2 h ago", "Edited yesterday", "Edited Oct 2". */
export function editedLabel(iso: string, now: Date = new Date()): string {
  const then = new Date(iso);
  if (Number.isNaN(then.getTime())) return 'Edited';
  const diffMs = now.getTime() - then.getTime();
  const min = Math.floor(diffMs / 60000);
  if (min < 1) return 'Edited just now';
  if (min < 60) return `Edited ${min} min ago`;
  const startOfDay = (d: Date) => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  const days = Math.round((startOfDay(now) - startOfDay(then)) / 86400000);
  if (days === 0) return `Edited ${Math.floor(min / 60)} h ago`;
  if (days === 1) return 'Edited yesterday';
  const base = `${MONTHS[then.getMonth()]} ${then.getDate()}`;
  return then.getFullYear() === now.getFullYear() ? `Edited ${base}` : `Edited ${base}, ${then.getFullYear()}`;
}

/** "9" -> version text for the footer: "8.1-essentials_build" -> "8.1". */
export function shortFfmpegVersion(version: string): string {
  return /^\d+(\.\d+)*/.exec(version)?.[0] ?? version;
}
