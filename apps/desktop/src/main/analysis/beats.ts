import { spawn } from 'node:child_process';
import { getFfmpeg } from '../ffmpeg.ts';
import { analyzePcm, type BeatMapData } from './beat-dsp.ts';

/**
 * Beat tracking without a Python sidecar (docs/DECISIONS.md #2): decode to 16 kHz mono, then
 * the pure DSP in beat-dsp.ts.
 */

export type { BeatMapData };

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

export async function detectBeats(file: string): Promise<BeatMapData> {
  const buf = await decodePcm16kMono(file);
  const n = Math.floor(buf.length / 2);
  const pcm = new Int16Array(buf.buffer.slice(buf.byteOffset, buf.byteOffset + n * 2)); // copy: a pooled Buffer may be unaligned
  return analyzePcm(pcm);
}
