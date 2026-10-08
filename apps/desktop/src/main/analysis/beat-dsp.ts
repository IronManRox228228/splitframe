/**
 * Beat tracking DSP (pure, no I/O): onset-strength envelope, tempo from autocorrelation refined
 * on a fine BPM grid, phase-locked beat grid, downbeats and coarse energy sections.
 * A pragmatic heuristic (docs/DECISIONS.md #2): honest about accuracy on weak-pulse tracks.
 */

export interface BeatMapData {
  bpm: number;
  beatsMs: number[];
  downbeatsMs: number[];
  sections: { startMs: number; endMs: number; label: string; energy: number }[];
}

export const BEAT_SAMPLE_RATE = 16000;
/** 8 ms hop: tempo and beat times are good to a few ms (the old 16 ms hop gave 121 BPM for a 120 BPM track) */
const HOP = 128;
const HOP_MS = (HOP / BEAT_SAMPLE_RATE) * 1000;

/**
 * Onset strength per hop. Frame i covers samples [i*HOP, (i+1)*HOP), so an attack inside it shows
 * up at index i; the sample before the first frame counts as silence, so a hit at t=0 registers.
 */
export function onsetEnvelope(pcm: Int16Array): Float32Array {
  const n = Math.floor(pcm.length / HOP);
  const rms = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    let sum = 0;
    for (let j = 0; j < HOP; j++) {
      const s = pcm[i * HOP + j]! / 32768;
      sum += s * s;
    }
    rms[i] = Math.sqrt(sum / HOP);
  }
  const onset = new Float32Array(n);
  for (let i = 0; i < n; i++) onset[i] = Math.max(0, rms[i]! - (i > 0 ? rms[i - 1]! : 0));
  return onset;
}

/** 3-tap box blur: lets a comb with a fractional period still land on an attack that straddles two hops. */
function blur3(env: Float32Array): Float32Array {
  const out = new Float32Array(env.length);
  for (let i = 0; i < env.length; i++) out[i] = (env[i - 1] ?? 0) + env[i]! + (env[i + 1] ?? 0);
  return out;
}

/** Comb score of a (fractional) period: the best phase and the onset energy summed along it. */
function combScore(env: Float32Array, periodHops: number): { score: number; phase: number } {
  let best = { score: -1, phase: 0 };
  for (let phase = 0; phase < periodHops; phase += 0.25) {
    let score = 0;
    for (let k = phase; k < env.length; k += periodHops) score += env[Math.round(k)] ?? 0;
    if (score > best.score) best = { score, phase };
  }
  return best;
}

/** Coarse tempo (BPM) by autocorrelation of the onset envelope, 60–190 BPM, mild preference for 90–150. */
function coarseTempo(env: Float32Array): number {
  const minLag = Math.floor(60000 / 190 / HOP_MS);
  const maxLag = Math.ceil(60000 / 60 / HOP_MS);
  let bestLag = minLag;
  let bestScore = 0;
  for (let lag = minLag; lag <= maxLag && lag < env.length; lag++) {
    let score = 0;
    for (let i = 0; i + lag < env.length; i++) score += env[i]! * env[i + lag]!;
    score /= env.length - lag;
    const bpm = 60000 / (lag * HOP_MS);
    const weight = bpm >= 90 && bpm <= 150 ? 1.15 : 1;
    if (score * weight > bestScore) {
      bestScore = score * weight;
      bestLag = lag;
    }
  }
  return 60000 / (bestLag * HOP_MS);
}

/** Tempo and phase together: fine BPM search around the coarse estimate, scored by the comb sum. */
function refineTempo(env: Float32Array, coarse: number): { bpm: number; phaseHops: number; score: number } {
  let best = { bpm: coarse, phaseHops: 0, score: -1 };
  const lo = coarse * 0.97;
  const hi = coarse * 1.03;
  for (let bpm = lo; bpm <= hi; bpm += 0.05) {
    const { score, phase } = combScore(env, 60000 / bpm / HOP_MS);
    if (score > best.score) best = { bpm, phaseHops: phase, score };
  }
  return best;
}

/**
 * Autocorrelation often lands on half the tempo (an accented bar-level pulse beats the beat
 * level). Take the doubled tempo when the in-between beats carry at least 60% of the on-beat
 * energy (a doubled comb scores 2x if they are as strong, 1x if they are empty).
 */
function pickOctave(env: Float32Array, coarse: number): { bpm: number; phaseHops: number } {
  let cur = refineTempo(env, coarse);
  while (cur.bpm * 2 <= 190) {
    const dbl = refineTempo(env, cur.bpm * 2);
    if (dbl.score < cur.score * 1.6) break;
    cur = dbl;
  }
  return cur;
}

export function analyzeOnsets(env: Float32Array, durationMs: number): BeatMapData {
  const coarse = coarseTempo(env);
  const { bpm, phaseHops } = pickOctave(blur3(env), coarse);
  const periodHops = 60000 / bpm / HOP_MS;
  // an attack inside frame i happened, on average, half a hop after the frame start
  const beatsMs: number[] = [];
  for (let k = phaseHops; k < env.length; k += periodHops) beatsMs.push(Math.round((k + 0.5) * HOP_MS));

  // downbeats: try offsets 0..3, pick the one whose beats carry the most onset energy
  let bestOffset = 0;
  let bestEnergy = -1;
  for (let offset = 0; offset < 4; offset++) {
    let energy = 0;
    for (let i = offset; i < beatsMs.length; i += 4) energy += env[Math.floor(beatsMs[i]! / HOP_MS)] ?? 0;
    if (energy > bestEnergy) {
      bestEnergy = energy;
      bestOffset = offset;
    }
  }
  const downbeatsMs = beatsMs.filter((_, i) => i % 4 === bestOffset);

  // sections: split the track where 2-second smoothed energy changes sharply
  const windowHops = Math.max(1, Math.floor(2000 / HOP_MS));
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
    const ms = i * HOP_MS;
    if (ms - sectionStart > 8000 && i > windowHops) {
      const change = Math.abs(smoothed[i]! - smoothed[i - windowHops]!) / Math.max(1e-6, maxE);
      if (change > 0.2) pushSection(ms);
    }
  }
  pushSection(durationMs);

  return { bpm: Number(bpm.toFixed(1)), beatsMs, downbeatsMs, sections };
}

/** Beat map of 16 kHz mono PCM. */
export function analyzePcm(pcm: Int16Array): BeatMapData {
  return analyzeOnsets(onsetEnvelope(pcm), (pcm.length / BEAT_SAMPLE_RATE) * 1000);
}
