import { spawn } from 'node:child_process';
import { getFfmpeg } from '../ffmpeg.ts';

/**
 * Beat tracking without a Python sidecar (docs/DECISIONS.md #2): decode to 16 kHz mono,
 * build an onset-strength envelope, autocorrelate for tempo, phase-lock a beat grid,
 * and mark downbeats + coarse sections by energy. This is a pragmatic DSP heuristic —
 * honest about accuracy on weak-pulse tracks (main prompt §1.2 "beat sync").
 */

export interface BeatMapData {
  bpm: number;
  beatsMs: number[];
  downbeatsMs: number[];
  sections: { startMs: number; endMs: number; label: string; energy: number }[];
}

async function decodePcm16kMono(file: string, maxSeconds = 600): Promise<Buffer> {
  const { ffmpegPath } = getFfmpeg();
  return new Promise((resolve, reject) => {
    const child = spawn(
      ffmpegPath,
      ['-i', file, '-ar', '16000', '-ac', '1', '-f', 's16le', '-t', String(maxSeconds), '-v', 'error', '-'],
      { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true },
    );
    const chunks: Buffer[] = [];
    child.stdout.on('data', (d) => chunks.push(d as Buffer));
    child.stderr.on('data', () => undefined);
    child.on('error', reject);
    child.on('close', (code) => {
      if (code !== 0) return reject(new Error(`ffmpeg audio decode failed (${code})`));
      resolve(Buffer.concat(chunks));
    });
  });
}

function onsetEnvelope(pcm: Buffer): { env: Float32Array; hopMs: number } {
  const hop = 256; // 16 ms at 16 kHz
  const n = Math.floor(pcm.length / 2 / hop);
  const env = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let sum = 0;
    const base = i * hop * 2;
    for (let j = 0; j < hop; j++) {
      const s = pcm.readInt16LE(base + j * 2) / 32768;
      sum += s * s;
    }
    env[i] = Math.sqrt(sum / hop);
  }
  // onset strength: half-wave rectified difference
  const onset = new Float32Array(n);
  for (let i = 1; i < n; i++) {
    onset[i] = Math.max(0, env[i]! - env[i - 1]!);
  }
  return { env: onset, hopMs: (hop / 16000) * 1000 };
}

function autocorrelationPeak(env: Float32Array, hopMs: number): number {
  // tempo search 60–190 BPM
  const minLag = Math.floor(60000 / 190 / hopMs);
  const maxLag = Math.ceil(60000 / 60 / hopMs);
  let bestLag = minLag;
  let bestScore = 0;
  for (let lag = minLag; lag <= maxLag && lag < env.length; lag++) {
    let score = 0;
    for (let i = 0; i + lag < env.length; i += 1) {
      score += env[i]! * env[i + lag]!;
    }
    score /= env.length - lag;
    // slight preference for 90–150 BPM range
    const bpm = 60000 / (lag * hopMs);
    const weight = bpm >= 90 && bpm <= 150 ? 1.15 : 1;
    if (score * weight > bestScore) {
      bestScore = score * weight;
      bestLag = lag;
    }
  }
  return 60000 / (bestLag * hopMs);
}

function phaseLockedBeats(env: Float32Array, hopMs: number, bpm: number): number[] {
  const periodHops = 60000 / bpm / hopMs;
  const beatMs: number[] = [];
  let bestPhase = 0;
  let bestEnergy = -1;
  for (let phase = 0; phase < periodHops; phase += 0.25) {
    let energy = 0;
    for (let k = phase; k < env.length; k += periodHops) {
      energy += env[Math.floor(k)] ?? 0;
    }
    if (energy > bestEnergy) {
      bestEnergy = energy;
      bestPhase = phase;
    }
  }
  for (let k = bestPhase; k < env.length; k += periodHops) {
    beatMs.push(Math.round(k * hopMs));
  }
  return beatMs;
}

export async function detectBeats(file: string): Promise<BeatMapData> {
  const pcm = await decodePcm16kMono(file);
  const { env, hopMs } = onsetEnvelope(pcm);
  const durationMs = (pcm.length / 2 / 16000) * 1000;

  const bpm = autocorrelationPeak(env, hopMs);
  const beatsMs = phaseLockedBeats(env, hopMs, bpm);

  // downbeats: try offsets 0..3, pick the one whose beats carry the most onset energy
  let bestOffset = 0;
  let bestEnergy = -1;
  for (let offset = 0; offset < 4; offset++) {
    let energy = 0;
    for (let i = offset; i < beatsMs.length; i += 4) {
      const hop = Math.floor(beatsMs[i]! / hopMs);
      energy += env[hop] ?? 0;
    }
    if (energy > bestEnergy) {
      bestEnergy = energy;
      bestOffset = offset;
    }
  }
  const downbeatsMs = beatsMs.filter((_, i) => i % 4 === bestOffset);

  // sections: split the track where 2-second smoothed energy changes sharply
  const windowHops = Math.max(1, Math.floor(2000 / hopMs));
  const smoothed = new Float32Array(env.length);
  let acc = 0;
  for (let i = 0; i < env.length; i++) {
    acc += env[i]!;
    if (i >= windowHops) acc -= env[i - windowHops]!;
    smoothed[i] = acc / Math.min(windowHops, i + 1);
  }
  let maxE = 0;
  for (const v of smoothed) maxE = Math.max(maxE, v);
  const sections: BeatMapData['sections'] = [];
  let sectionStart = 0;
  let sectionEnergyAcc = 0;
  let sectionEnergyN = 0;
  const pushSection = (endMs: number) => {
    const energy = sectionEnergyN > 0 ? sectionEnergyAcc / sectionEnergyN / Math.max(1e-6, maxE) : 0;
    sections.push({
      startMs: sectionStart,
      endMs,
      label: energy > 0.66 ? 'high' : energy > 0.33 ? 'medium' : 'low',
      energy: Number(energy.toFixed(3)),
    });
    sectionStart = endMs;
    sectionEnergyAcc = 0;
    sectionEnergyN = 0;
  };
  for (let i = 0; i < smoothed.length; i++) {
    sectionEnergyAcc += smoothed[i]!;
    sectionEnergyN++;
    const ms = i * hopMs;
    if (ms - sectionStart > 8000 && i > windowHops) {
      const change = Math.abs(smoothed[i]! - smoothed[i - windowHops]!) / Math.max(1e-6, maxE);
      if (change > 0.2) pushSection(ms);
    }
  }
  pushSection(durationMs);

  return { bpm: Number(bpm.toFixed(1)), beatsMs, downbeatsMs, sections };
}
