/**
 * Pure helpers for locating and choosing ffmpeg pieces. Kept free of Electron imports so
 * they can be unit-tested under plain Node.
 */

/** Executable file name for a bundled/PATH binary on the given platform. */
export function binaryFileName(name: string, platform: NodeJS.Platform = process.platform): string {
  return platform === 'win32' && !name.toLowerCase().endsWith('.exe') ? `${name}.exe` : name;
}

export const H264_PREFS: Record<string, string[]> = {
  darwin: ['h264_videotoolbox', 'libopenh264', 'mpeg4'],
  win32: ['h264_nvenc', 'h264_qsv', 'h264_amf', 'libopenh264', 'mpeg4'],
  linux: ['h264_nvenc', 'h264_qsv', 'h264_amf', 'libopenh264', 'mpeg4'],
};

/**
 * Pick the first preferred encoder that is both compiled into the build (`-encoders`
 * output) and actually usable on this machine. `-encoders` lists hardware encoders even
 * when no matching GPU exists, so each candidate must pass `probe` (a real trial encode).
 */
export function chooseH264Encoder(
  encodersOutput: string,
  platform: string,
  probe: (encoder: string) => boolean,
): string {
  const prefs = H264_PREFS[platform] ?? H264_PREFS['linux']!;
  const listed = (enc: string) => encodersOutput.includes(` ${enc} `);
  for (const enc of prefs) {
    if (listed(enc) && probe(enc)) return enc;
  }
  if (listed('libx264') && probe('libx264')) return 'libx264'; // dev PATH ffmpeg only
  return 'mpeg4';
}
