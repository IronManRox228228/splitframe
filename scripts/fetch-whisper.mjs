#!/usr/bin/env node
/**
 * Download prebuilt whisper.cpp CLI for bundling into apps/desktop/bin/<plat>-<arch>/.
 * Dev machines can simply `brew install whisper-cpp` — the app falls back to PATH.
 * macOS: whisper.cpp GitHub releases (whisper-bins). Windows/Linux: build from source
 * (cmake -DWHISPER_BUILD_TESTS=OFF) or place a static binary in bin/.
 */
import { mkdirSync, createWriteStream, existsSync, chmodSync, unlinkSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { pipeline } from 'node:stream/promises';
import { execSync } from 'node:child_process';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const plat = process.platform;
const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
const outDir = join(root, 'apps/desktop/bin', `${plat}-${arch}`);
const VERSION = 'v1.7.4';

if (plat !== 'darwin') {
  console.error(`No prebuilt whisper-cli recipe for ${plat}. Build from source into ${outDir}.`);
  process.exit(1);
}

const asset = `https://github.com/ggml-org/whisper.cpp/releases/download/${VERSION}/whisper-bins-${VERSION}-macos-${arch}.zip`;
mkdirSync(outDir, { recursive: true });
if (existsSync(join(outDir, 'whisper-cli'))) {
  console.log(`✓ whisper-cli already present in ${outDir}`);
  process.exit(0);
}
console.log(`↓ ${asset}`);
const res = await fetch(asset, { redirect: 'follow' });
if (!res.ok || !res.body) {
  console.error(`HTTP ${res.status} — check ${asset}`);
  process.exit(1);
}
const archive = join(outDir, 'whisper.zip');
await pipeline(res.body, createWriteStream(archive));
execSync(`unzip -o ${archive} -d ${outDir}`, { stdio: 'inherit' });
unlinkSync(archive);
const p = join(outDir, 'whisper-cli');
if (existsSync(p)) chmodSync(p, 0o755);
console.log(`✓ done → ${outDir}`);
