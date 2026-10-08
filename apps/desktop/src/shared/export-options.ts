/** Export option maths shared by the main process (what gets rendered) and the dialog (what it promises). */

export type ExportQuality = '720p' | '1080p' | '1440p';
export type ExportFormat = 'mp4' | 'webm';

export const EXPORT_QUALITIES: readonly ExportQuality[] = ['720p', '1080p', '1440p'];
const SHORT_SIDE: Record<ExportQuality, number> = { '720p': 720, '1080p': 1080, '1440p': 1440 };

export interface ResolvedExport {
  name: string;
  width: number;
  height: number;
  format: ExportFormat;
  videoBitrateK: number;
}

const even = (n: number) => Math.max(2, Math.round(n / 2) * 2);

/**
 * Quality names the SHORT side of the picture (as video sites do), so a 9:16 short at
 * "1080p" is 1080x1920 and a 16:9 video is 1920x1080. Bitrate scales with pixel count.
 */
export function resolveExport(quality: ExportQuality, format: ExportFormat, canvasW: number, canvasH: number): ResolvedExport {
  const short = SHORT_SIDE[quality];
  const scale = short / Math.max(1, Math.min(canvasW, canvasH));
  const width = even(canvasW * scale);
  const height = even(canvasH * scale);
  const base = format === 'webm' ? 10000 : 12000;
  const videoBitrateK = Math.max(1500, Math.round((base * (width * height)) / (1920 * 1080) / 100) * 100);
  return { name: `${quality} ${format.toUpperCase()}`, width, height, format, videoBitrateK };
}

/** Rough output size: video bitrate plus 128 kbps audio over the duration. */
export function estimateBytes(videoBitrateK: number, durationSec: number): number {
  return Math.round(((videoBitrateK + 128) * 1000 * durationSec) / 8);
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.max(1, Math.round(bytes / 1024))} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${bytes / (1024 * 1024) < 10 ? (bytes / (1024 * 1024)).toFixed(1) : Math.round(bytes / (1024 * 1024))} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
}

/** A safe base file name: no path separators or characters Windows rejects, no reserved device names. */
export function sanitizeFileName(raw: string): string {
  let s = raw
    // eslint-disable-next-line no-control-regex
    .replace(/[<>:"/\\|?*\u0000-\u001f]/g, '')
    .replace(/\s+/g, ' ')
    .trim()
    .replace(/[. ]+$/, '')
    .replace(/^\.+/, '');
  if (/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\..*)?$/i.test(s)) s = `_${s}`;
  return s.slice(0, 120).trim();
}

export interface ExportPreset {
  name: string;
  width: number;
  height: number;
  format: ExportFormat;
  videoBitrateK: number;
}

/** Fixed platform presets (the 1080p-class ones keep their canvas-independent size). */
export const EXPORT_PRESETS: ExportPreset[] = [
  { name: 'TikTok / Reels / Shorts', width: 1080, height: 1920, format: 'mp4', videoBitrateK: 12000 },
  { name: 'YouTube 1080p', width: 1920, height: 1080, format: 'mp4', videoBitrateK: 12000 },
  { name: 'Square 1080', width: 1080, height: 1080, format: 'mp4', videoBitrateK: 10000 },
  { name: '1440p', width: 2560, height: 1440, format: 'mp4', videoBitrateK: 20000 },
  { name: 'WebM 1080p', width: 1920, height: 1080, format: 'webm', videoBitrateK: 10000 },
];

/**
 * Everything the agent can name: the platform presets plus every quality tier (720p / 1080p /
 * 1440p, MP4 and WebM) from resolveExport, the same maths the export dialog uses. Tiers size
 * the short side of the current canvas.
 */
export function listPresets(canvasW: number, canvasH: number): ExportPreset[] {
  const tiers = EXPORT_QUALITIES.flatMap((q) => (['mp4', 'webm'] as const).map((f) => resolveExport(q, f, canvasW, canvasH)));
  return [...EXPORT_PRESETS, ...tiers];
}

/** Find a preset by name, case-insensitive; a bare tier such as "720p" means its MP4. */
export function findPreset(name: string, canvasW: number, canvasH: number): ExportPreset | undefined {
  const all = listPresets(canvasW, canvasH);
  const key = name.trim().toLowerCase();
  return all.find((p) => p.name.toLowerCase() === key) ?? all.find((p) => p.name.toLowerCase() === `${key} mp4`);
}
