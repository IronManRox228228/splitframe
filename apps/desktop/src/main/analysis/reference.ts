import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { runFfmpeg } from '../ffmpeg.ts';
import { detectScenes } from './scenes.ts';
import { getAsset } from '../asset-service.ts';

/**
 * Reference style profile (main prompt §1.2, milestone 8): analyze the reference video's
 * cut rhythm and rough color stats. The reference is NEVER used in output — only its
 * measured properties are copied onto the user's edit.
 */

export interface ReferenceStyleProfile {
  assetId: string;
  analyzedAt: string;
  durationMs: number;
  sceneCount: number;
  avgShotLengthSec: number;
  cutsPerMinute: number;
  /** luma/saturation averages from ffmpeg signalstats (0..255 / 0..255) */
  color: { lumaAvg: number; saturationAvg: number };
  /** sampled frame paths (project cache) the agent can request */
  notes: string[];
}

const db = () => import('../db.ts').then(({ getDb }) => getDb());

export async function analyzeReferenceStyle(assetId: string, cacheDir: string): Promise<ReferenceStyleProfile> {
  const asset = getAsset(assetId);
  if (!asset) throw new Error(`Reference asset ${assetId} not found`);
  const scenes = await detectScenes(asset.path, asset.durationMs, { threshold: 0.2 });
  const sceneCount = Math.max(1, scenes.length);
  const avgShotLengthSec = asset.durationMs / 1000 / sceneCount;
  const cutsPerMinute = sceneCount / Math.max(1e-6, asset.durationMs / 60000);

  // coarse color stats: signalstats YAVG + SATAVG sampled every ~2s
  let lumaSum = 0;
  let satSum = 0;
  let samples = 0;
  let stderrAll = '';
  await runFfmpeg([
    '-i', asset.path,
    '-vf', "select='not(mod(n,60))',signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-,metadata=print:key=lavfi.signalstats.SATAVG:file=-",
    '-f', 'null', '-',
  ], { onStderr: (t) => (stderrAll += t) });
  for (const match of stderrAll.matchAll(/YAVG=(-?\d+(?:\.\d+)?)/g)) {
    lumaSum += Number(match[1]);
    samples++;
  }
  for (const match of stderrAll.matchAll(/SATAVG=(-?\d+(?:\.\d+)?)/g)) {
    satSum += Number(match[1]);
  }

  const profile: ReferenceStyleProfile = {
    assetId,
    analyzedAt: new Date().toISOString(),
    durationMs: asset.durationMs,
    sceneCount,
    avgShotLengthSec: Number(avgShotLengthSec.toFixed(2)),
    cutsPerMinute: Number(cutsPerMinute.toFixed(1)),
    color: {
      lumaAvg: samples > 0 ? Number((lumaSum / samples).toFixed(1)) : 128,
      saturationAvg: samples > 0 ? Number((satSum / samples).toFixed(1)) : 64,
    },
    notes: [
      'Color stats are coarse global averages sampled every ~2s — a starting point for grading, not a LUT.',
      'Caption/font style of the reference is not auto-detected; describe it from frames via captureFrame-style tools instead.',
    ],
  };

  const database = await db();
  database
    .prepare(`CREATE TABLE IF NOT EXISTS reference_profiles (asset_id TEXT PRIMARY KEY, profile TEXT NOT NULL)`)
    .run();
  database.prepare(`INSERT OR REPLACE INTO reference_profiles (asset_id, profile) VALUES (?, ?)`).run(assetId, JSON.stringify(profile));
  void cacheDir;
  return profile;
}

export async function getReferenceProfile(assetId: string): Promise<ReferenceStyleProfile | null> {
  const database = await db();
  try {
    const row = database.prepare(`SELECT profile FROM reference_profiles WHERE asset_id=?`).get(assetId) as { profile: string } | undefined;
    return row ? (JSON.parse(row.profile) as ReferenceStyleProfile) : null;
  } catch {
    return null;
  }
}

export async function saveOtio(name: string, json: unknown, dir: string): Promise<string> {
  const dest = join(dir, 'exports', `${name}.otio`);
  await writeFile(dest, JSON.stringify(json, null, 1), 'utf8');
  return dest;
}

void readFile;
