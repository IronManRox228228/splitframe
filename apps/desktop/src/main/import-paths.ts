import { statSync } from 'node:fs';
import { basename, isAbsolute, relative } from 'node:path';

/** Supported media extensions (browser-playable + common camera formats via proxies). */
export const MEDIA_EXTENSION_LIST = [
  'mp4', 'mov', 'm4v', 'webm', 'mkv', 'avi', 'gif',
  'mp3', 'wav', 'm4a', 'aac', 'ogg', 'flac', 'aiff',
  'png', 'jpg', 'jpeg', 'webp', 'bmp',
] as const;

const MEDIA_EXTENSIONS = new Set<string>(MEDIA_EXTENSION_LIST);

export function isMediaFile(path: string): boolean {
  const name = basename(path);
  const dot = name.lastIndexOf('.');
  if (dot <= 0) return false;
  return MEDIA_EXTENSIONS.has(name.slice(dot + 1).toLowerCase());
}

export interface PathClassification {
  /** absolute paths that exist, are regular files and have a supported extension (deduplicated) */
  accepted: string[];
  /** file names that exist but are not supported media (or are directories / unreadable) */
  skipped: string[];
}

/**
 * Re-check paths that came from the renderer (drag-and-drop) before importing: they must be
 * absolute, point at an existing regular file and carry a supported media extension.
 */
export function classifyImportPaths(paths: string[], stat: (p: string) => { isFile(): boolean } | null = safeStat): PathClassification {
  const accepted: string[] = [];
  const skipped: string[] = [];
  const seen = new Set<string>();
  for (const p of paths) {
    if (seen.has(p)) continue;
    seen.add(p);
    const name = basename(p) || p;
    if (!isAbsolute(p) || p.includes('\0')) {
      skipped.push(name);
      continue;
    }
    const st = stat(p);
    if (!st || !st.isFile() || !isMediaFile(p)) {
      skipped.push(name);
      continue;
    }
    accepted.push(p);
  }
  return { accepted, skipped };
}

function safeStat(p: string): { isFile(): boolean } | null {
  try {
    return statSync(p);
  } catch {
    return null;
  }
}

/** True when `file` lies strictly inside `root` (path.relative is case-insensitive on Windows). */
export function isUnder(root: string, file: string): boolean {
  const rel = relative(root, file);
  return rel !== '' && !rel.startsWith('..') && !isAbsolute(rel);
}

/**
 * The generated files of an asset that are safe to delete: only files inside the owning
 * project's folder, never the original media.
 */
export function removableGeneratedFiles(
  asset: { path: string; proxyPath?: string | undefined; thumbPath?: string | undefined; waveformPath?: string | undefined },
  keyframes: string[],
  projectDir: string,
): string[] {
  const candidates = [asset.proxyPath, asset.thumbPath, asset.waveformPath, ...keyframes];
  return candidates.filter((f): f is string => Boolean(f) && f !== asset.path && isUnder(projectDir, f as string));
}

export interface ImportSummary {
  /** every asset row created (including ones whose probe failed, so cards show up) */
  assets: unknown[];
  imported: number;
  /** names of files that were not supported media */
  skipped: string[];
  failed: { name: string; error: string }[];
}
