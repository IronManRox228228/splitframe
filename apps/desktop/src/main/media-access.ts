import { isAbsolute, resolve, sep } from 'node:path';

/**
 * Which local files the cbmedia:// protocol may serve. The renderer is untrusted, so it
 * must not be able to read arbitrary files: only files inside the Cutboard project
 * folders, or files that are registered as an asset's original/proxy/thumbnail/waveform.
 */

const norm = (p: string, platform: NodeJS.Platform) => (platform === 'win32' ? resolve(p).toLowerCase() : resolve(p));

export function isInsideRoot(filePath: string, root: string, platform: NodeJS.Platform = process.platform): boolean {
  if (!isAbsolute(filePath)) return false;
  const file = norm(filePath, platform);
  const base = norm(root, platform);
  return file === base || file.startsWith(base.endsWith(sep) ? base : base + sep);
}

export function isMediaPathAllowed(
  filePath: string,
  opts: { roots: string[]; isKnownAssetPath: (path: string) => boolean; platform?: NodeJS.Platform },
): boolean {
  if (!isAbsolute(filePath)) return false;
  const platform = opts.platform ?? process.platform;
  if (opts.roots.some((root) => isInsideRoot(filePath, root, platform))) return true;
  return opts.isKnownAssetPath(filePath);
}

/**
 * Full cbmedia check, given the requested path and its symlink-resolved form. A registered
 * asset is matched on the exact path stored at import (the user picked that file), because
 * its resolved form can legitimately differ (8.3 short names, symlinks, mapped drives, case).
 * Anything allowed only by living under a project root must still be under a root once
 * resolved, so a symlink inside a project cannot point elsewhere.
 */
export function canServeMediaPath(
  filePath: string,
  realPath: string,
  opts: { roots: string[]; realRoots: string[]; isKnownAssetPath: (path: string) => boolean; platform?: NodeJS.Platform },
): boolean {
  if (!isAbsolute(filePath)) return false;
  if (opts.isKnownAssetPath(filePath)) return true;
  const platform = opts.platform ?? process.platform;
  const under = (p: string, roots: string[]) => roots.some((root) => isInsideRoot(p, root, platform));
  return under(filePath, opts.roots) && (under(realPath, opts.realRoots) || under(realPath, opts.roots));
}

/**
 * Parse a single-range `Range: bytes=...` header against a file of `size` bytes. Returns the
 * inclusive byte span to serve as 206, 'unsatisfiable' for a 416, or null to serve the whole
 * file as 200 (no header, a multi-range request, or one we don't understand).
 */
export function parseByteRange(header: string | null, size: number): { start: number; end: number } | 'unsatisfiable' | null {
  const m = header ? /^bytes=(\d*)-(\d*)$/.exec(header.trim()) : null;
  if (!m || (m[1] === '' && m[2] === '')) return null;
  if (m[1] === '') {
    // suffix range: the last N bytes
    const n = Number(m[2]);
    if (n === 0 || size === 0) return 'unsatisfiable';
    return { start: Math.max(0, size - n), end: size - 1 };
  }
  const start = Number(m[1]);
  if (start >= size) return 'unsatisfiable';
  const end = m[2] === '' ? size - 1 : Math.min(Number(m[2]), size - 1);
  return end < start ? null : { start, end };
}
