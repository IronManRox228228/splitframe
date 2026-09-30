import { app } from 'electron';
import { join } from 'node:path';
import { mkdirSync } from 'node:fs';

/** Central filesystem layout for the app (addendum §2: local files, no cloud). */
export interface AppPaths {
  /** app support dir (database, settings, logs) */
  userData: string;
  /** root of user-visible projects */
  projectsRoot: string;
  /** database file */
  dbPath: string;
  /** bundled ffmpeg dir: <resources>/bin/<plat>-<arch>/ or app/bin/... in dev */
  bundledBinDir: string;
}

let paths: AppPaths | null = null;

export function getPaths(): AppPaths {
  if (paths) return paths;
  const userData = app.getPath('userData');
  const projectsRoot = join(app.getPath('videos'), 'Cutboard');
  const dbPath = join(userData, 'cutboard.sqlite');
  // in dev: apps/desktop/bin/<plat>-<arch>; when packaged: <resources>/bin/<plat>-<arch>
  const plat = process.platform;
  const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
  const isPackaged = app.isPackaged;
  const bundledBinDir = isPackaged
    ? join(process.resourcesPath ?? '', 'bin', `${plat}-${arch}`)
    : join(app.getAppPath(), 'bin', `${plat}-${arch}`);
  mkdirSync(projectsRoot, { recursive: true });
  mkdirSync(userData, { recursive: true });
  paths = { userData, projectsRoot, dbPath, bundledBinDir };
  return paths;
}

export function projectDir(projectsRoot: string, projectId: string, name: string): string {
  const slug =
    name
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/^-+|-+$/g, '')
      .slice(0, 40) || 'project';
  const dir = join(projectsRoot, `${slug}-${projectId.slice(4, 12)}`);
  mkdirSync(join(dir, 'cache'), { recursive: true });
  mkdirSync(join(dir, 'exports'), { recursive: true });
  return dir;
}
