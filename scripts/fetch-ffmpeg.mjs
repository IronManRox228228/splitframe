#!/usr/bin/env node
/**
 * Download static ffmpeg/ffprobe builds per OS/arch into apps/desktop/bin/<plat>-<arch>/.
 * Sources (LGPL builds, no GPL components — docs/DECISIONS.md #5):
 *   macOS:  evermeet.cx (LGPL build)
 *   win:    gyan.dev release-lgpl
 *   linux:  johnvansickle.com release-lgpl (or a distro build placed manually)
 * Checksums: pinned per release — update the URLs + shas when bumping versions.
 */
import { mkdirSync, createWriteStream, existsSync, chmodSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { pipeline } from 'node:stream/promises';
import { execSync } from 'node:child_process';
import readline from 'node:readline';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const plat = process.platform;
const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
const outDir = join(root, 'apps/desktop/bin', `${plat}-${arch}`);

// NOTE: verify these URLs + checksums before relying on them for packaged builds.
const SOURCES = {
  'darwin-arm64': {
    ffmpeg: 'https://evermeet.cx/ffmpeg/get/ffmpeg/zip',
    ffprobe: 'https://evermeet.cx/ffmpeg/get/ffprobe/zip',
    extract: 'unzip -o {out} -d {dir}',
  },
  'darwin-x64': {
    ffmpeg: 'https://evermeet.cx/ffmpeg/get/ffmpeg/zip',
    ffprobe: 'https://evermeet.cx/ffmpeg/get/ffprobe/zip',
    extract: 'unzip -o {out} -d {dir}',
  },
  'win32-x64': {
    ffmpeg: 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-lgpl.zip',
    ffprobe: null, // included in the gyan zip
    extract: 'unzip -o {out} -d {dir} && find {dir} -name ffmpeg.exe -exec cp {{}} ' + outDir + ' \\;',
  },
  'linux-x64': {
    ffmpeg: 'https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz',
    ffprobe: null,
    extract: 'tar -xJf {out} -C {dir} --strip-components=1 --wildcards "*/ffmpeg" "*/ffprobe"',
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
  mkdirSync(outDir, { recursive: true });
  if (existsSync(join(outDir, plat === 'win32' ? 'ffmpeg.exe' : 'ffmpeg'))) {
    console.log(`✓ ffmpeg already present in ${outDir}`);
    return;
  }
  const { default: tmp } = await import('node:fs');
  const archive = join(outDir, 'download.bin');
  try {
    await download(source.ffmpeg, archive);
    const cmd = source.extract.replaceAll('{out}', archive).replaceAll('{dir}', outDir);
    console.log(`$ ${cmd}`);
    execSync(cmd, { stdio: 'inherit' });
    if (source.ffprobe) {
      const probeArchive = join(outDir, 'probe.zip');
      await download(source.ffprobe, probeArchive);
      execSync(`unzip -o ${probeArchive} -d ${outDir}`, { stdio: 'inherit' });
      tmp.unlinkSync(probeArchive);
    }
    tmp.unlinkSync(archive);
    for (const bin of ['ffmpeg', 'ffprobe']) {
      const p = join(outDir, plat === 'win32' ? `${bin}.exe` : bin);
      if (existsSync(p)) chmodSync(p, 0o755);
    }
    console.log(`✓ done → ${outDir}`);
  } catch (err) {
    console.error('Download failed:', err.message);
    console.error('For development you can simply install ffmpeg via your package manager.');
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    rl.close();
    process.exit(1);
  }
}

main();
