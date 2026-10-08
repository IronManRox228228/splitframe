import { app } from 'electron';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

/**
 * Which GPU the editor window renders on. Heavy video work (proxy decode, scene
 * detection, export encode) already runs on the discrete GPU through ffmpeg (CUDA/NVENC)
 * whatever this says. On hybrid laptops the display hangs off the integrated GPU, so
 * forcing the window onto the discrete one adds a cross-GPU copy per frame: measured on
 * an RTX 4060 laptop, preview playback dropped from 60 to 40 fps. So the default leaves
 * the choice to the OS (and the user's Windows graphics settings); forcing it is opt-in.
 */
export type GpuPreference = 'auto' | 'high-performance';

export function parseGpuPreference(value: unknown): GpuPreference {
  return value === 'high-performance' ? 'high-performance' : 'auto';
}

/**
 * Chromium picks its GPU at startup, before settings load asynchronously, so this reads
 * the one value it needs straight from settings.json. Any read or parse failure means auto.
 */
export function readGpuPreferenceSync(): GpuPreference {
  try {
    const raw = readFileSync(join(app.getPath('userData'), 'settings.json'), 'utf8');
    return parseGpuPreference((JSON.parse(raw) as { gpu?: { preference?: unknown } }).gpu?.preference);
  } catch {
    return 'auto';
  }
}

/** Must run before the app is ready. Only matters on machines with more than one GPU. */
export function applyGpuPreference(pref: GpuPreference): void {
  if (pref === 'high-performance') app.commandLine.appendSwitch('force_high_performance_gpu');
}
