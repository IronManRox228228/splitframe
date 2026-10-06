#!/usr/bin/env node
/**
 * Download a prebuilt whisper.cpp CLI for bundling into apps/desktop/bin/<plat>-<arch>/.
 * Dev machines can also install it themselves - the app falls back to PATH (`brew install whisper-cpp`).
 *
 *   Windows x64: whisper-bin-x64.zip from the whisper.cpp release (CPU build)
 *   Linux x64:   whisper-bin-ubuntu-x64.tar.gz from the same release (not exercised on a Linux machine here)
 *   macOS:       whisper.cpp publishes no macOS CLI binaries (only an xcframework), so there is
 *                nothing to download: `brew install whisper-cpp`, or build from source
 *                (cmake -B build -DWHISPER_BUILD_TESTS=OFF && cmake --build build -j --config Release)
 *                and copy whisper-cli into the directory below.
 *
 * Models are separate: download them from the app (AI settings -> Local models).
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
const VERSION = 'v1.9.2';
const RELEASE = `https://github.com/ggml-org/whisper.cpp/releases/download/${VERSION}`;

const SOURCES = {
  'win32-x64': {
    url: `${RELEASE}/whisper-bin-x64.zip`,
    archive: 'whisper.zip',
    binary: 'whisper-cli.exe',
    // the CLI loads whisper.dll and the ggml*.dll backends from its own folder
    extract: (archive, dir) =>
      execFileSync(
        'powershell',
        [
          '-NoProfile',
          '-Command',
          "$ErrorActionPreference = 'Stop'; $tmp = Join-Path $env:CB_DIR 'whisper-x'; " +
            'Expand-Archive -Force -LiteralPath $env:CB_ARCHIVE -DestinationPath $tmp; ' +
            "Get-ChildItem -Recurse -LiteralPath $tmp | Where-Object { $_.Name -eq 'whisper-cli.exe' -or ($_.Extension -eq '.dll' -and $_.Name -ne 'SDL2.dll') } | Copy-Item -Destination $env:CB_DIR -Force; " +
            'Remove-Item -Recurse -Force -LiteralPath $tmp',
        ],
        { stdio: 'inherit', env: { ...process.env, CB_ARCHIVE: archive, CB_DIR: dir } },
      ),
  },
  'linux-x64': {
    url: `${RELEASE}/whisper-bin-ubuntu-x64.tar.gz`,
    archive: 'whisper.tar.gz',
    binary: 'whisper-cli',
    // whisper-cli plus the shared libraries it links against
    extract: (archive, dir) =>
      execFileSync('tar', ['-xzf', archive, '-C', dir, '--strip-components=1', '--wildcards', '*/whisper-cli', '*/lib*.so*'], { stdio: 'inherit' }),
  },
};

const key = `${plat}-${arch}`;
const source = SOURCES[key];
if (!source) {
  console.error(
    `No prebuilt whisper-cli is published for ${key}.\n` +
      '  - macOS: `brew install whisper-cpp` (the app finds it on PATH), or build whisper.cpp from source.\n' +
      `  - Otherwise build from source and place whisper-cli in ${outDir}\n` +
      'Without it the app still works, but footage gets no transcript (captions, silence removal and transcript search need one).',
  );
  process.exit(1);
}

mkdirSync(outDir, { recursive: true });
if (existsSync(join(outDir, source.binary))) {
  console.log(`✓ ${source.binary} already present in ${outDir}`);
  process.exit(0);
}
console.log(`↓ ${source.url}`);
const res = await fetch(source.url, { redirect: 'follow' });
if (!res.ok || !res.body) {
  console.error(`HTTP ${res.status} - check ${source.url}`);
  process.exit(1);
}
const archive = join(outDir, source.archive);
await pipeline(res.body, createWriteStream(archive));
try {
  source.extract(archive, outDir);
} finally {
  unlinkSync(archive);
}
const p = join(outDir, source.binary);
if (existsSync(p)) chmodSync(p, 0o755);
console.log(`✓ done → ${outDir}`);
