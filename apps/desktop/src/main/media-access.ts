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
