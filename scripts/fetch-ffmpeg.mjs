#!/usr/bin/env node
/**
 * Download static ffmpeg/ffprobe builds per OS/arch into apps/desktop/bin/<plat>-<arch>/.
 * Sources:
 *   win/linux: BtbN/FFmpeg-Builds, `-lgpl` variants (no GPL components - docs/DECISIONS.md #5)
 *   macOS:     no LGPL build is published anywhere we could verify. evermeet.cx ships GPL builds
 *              (--enable-gpl, libx264/x265), so they are only fetched with `--allow-gpl` for local
 *              development and must NOT be bundled into a distributed app. Without the flag the
 *              script explains the alternatives (build with --disable-gpl, or use ffmpeg on PATH).
 * These URLs follow BtbN's rolling `latest` release, so downloads are not checksum-pinned.
 */
import { mkdirSync, createWriteStream, existsSync, chmodSync, unlinkSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { pipeline } from 'node:stream/promises';
import { execFileSync } from 'node:child_process';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const plat = process.platform;
const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
const outDir = join(root, 'apps/desktop/bin', `${plat}-${arch}`);
const allowGpl = process.argv.includes('--allow-gpl');

const BTBN = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest';

// Archives are extracted with argument arrays (no shell), so paths with spaces or quotes are safe.
const SOURCES = {
  'darwin-arm64': {
    gpl: true,
    ffmpeg: 'https://evermeet.cx/ffmpeg/get/ffmpeg/zip',
    ffprobe: 'https://evermeet.cx/ffmpeg/get/ffprobe/zip',
    extract: (archive, dir) => execFileSync('unzip', ['-o', archive, '-d', dir], { stdio: 'inherit' }),
  },
  'darwin-x64': {
    gpl: true,
    ffmpeg: 'https://evermeet.cx/ffmpeg/get/ffmpeg/zip',
    ffprobe: 'https://evermeet.cx/ffmpeg/get/ffprobe/zip',
    extract: (archive, dir) => execFileSync('unzip', ['-o', archive, '-d', dir], { stdio: 'inherit' }),
  },
  'win32-x64': {
    ffmpeg: `${BTBN}/ffmpeg-n8.1-latest-win64-lgpl-8.1.zip`,
    ffprobe: null, // included in the zip
    extract: (archive, dir) =>
      // paths travel through the environment, so quoting in the script cannot be broken by them
      execFileSync(
        'powershell',
        [
          '-NoProfile',
          '-Command',
          "$ErrorActionPreference = 'Stop'; $tmp = Join-Path $env:CB_DIR 'x'; " +
            'Expand-Archive -Force -LiteralPath $env:CB_ARCHIVE -DestinationPath $tmp; ' +
            "Get-ChildItem -Recurse -LiteralPath $tmp | Where-Object { $_.Name -in 'ffmpeg.exe','ffprobe.exe' } | Copy-Item -Destination $env:CB_DIR; " +
            'Remove-Item -Recurse -Force -LiteralPath $tmp',
        ],
        { stdio: 'inherit', env: { ...process.env, CB_ARCHIVE: archive, CB_DIR: dir } },
      ),
  },
  'linux-x64': {
    ffmpeg: `${BTBN}/ffmpeg-n8.1-latest-linux64-lgpl-8.1.tar.xz`,
    ffprobe: null,
    extract: (archive, dir) =>
      execFileSync('tar', ['-xJf', archive, '-C', dir, '--strip-components=2', '--wildcards', '*/bin/ffmpeg', '*/bin/ffprobe'], { stdio: 'inherit' }),
  },
  'linux-arm64': {
    ffmpeg: `${BTBN}/ffmpeg-n8.1-latest-linuxarm64-lgpl-8.1.tar.xz`,
    ffprobe: null,
    extract: (archive, dir) =>
      execFileSync('tar', ['-xJf', archive, '-C', dir, '--strip-components=2', '--wildcards', '*/bin/ffmpeg', '*/bin/ffprobe'], { stdio: 'inherit' }),
  },
};

async function download(url, dest) {
  mkdirSync(dirname(dest), { recursive: true });
  console.log(`↓ ${url}`);
  const res = await fetch(url, { redirect: 'follow' });
  if (!res.ok || !res.body) throw new Error(`HTTP ${res.status} for ${url}`);
  await pipeline(res.body, createWriteStream(dest));
}

async function main() {
  const key = `${plat}-${arch}`;
  const source = SOURCES[key];
  if (!source) {
    console.error(`No prebuilt ffmpeg source for ${key}. Install ffmpeg on PATH for development.`);
    process.exit(1);
  }
  if (source.gpl && !allowGpl) {
    console.error(
      `No LGPL ffmpeg build is available for ${key}.\n` +
        '  - For development, install ffmpeg on PATH (e.g. `brew install ffmpeg`); the app falls back to it.\n' +
        '  - To bundle one, build ffmpeg yourself with --disable-gpl --disable-nonfree and place ffmpeg/ffprobe in\n' +
        `    ${outDir}\n` +
        '  - To download evermeet.cx\'s GPL build for LOCAL development only (never ship it), run:\n' +
        '    pnpm fetch:ffmpeg -- --allow-gpl',
    );
    process.exit(1);
  }
  if (source.gpl) console.warn('! GPL build: for local development only - do not distribute an app that bundles it.');
  mkdirSync(outDir, { recursive: true });
  if (existsSync(join(outDir, plat === 'win32' ? 'ffmpeg.exe' : 'ffmpeg'))) {
    console.log(`✓ ffmpeg already present in ${outDir}`);
    return;
  }
  // Expand-Archive (win) refuses anything without a .zip extension.
  const archive = join(outDir, plat === 'win32' ? 'download.zip' : 'download.bin');
  try {
    await download(source.ffmpeg, archive);
    source.extract(archive, outDir);
    if (source.ffprobe) {
      const probeArchive = join(outDir, 'probe.zip');
      await download(source.ffprobe, probeArchive);
      source.extract(probeArchive, outDir);
      unlinkSync(probeArchive);
    }
    unlinkSync(archive);
    for (const bin of ['ffmpeg', 'ffprobe']) {
      const p = join(outDir, plat === 'win32' ? `${bin}.exe` : bin);
      if (existsSync(p)) chmodSync(p, 0o755);
    }
    console.log(`✓ done → ${outDir}`);
  } catch (err) {
    console.error('Download failed:', err.message);
    console.error('For development you can simply install ffmpeg via your package manager.');
    process.exit(1);
  }
}

main();
