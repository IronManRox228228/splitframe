import { describe, expect, it } from 'vitest';
import { binaryFileName, chooseH264Encoder } from './ffmpeg-utils.ts';

describe('binaryFileName', () => {
  it('adds .exe on Windows only', () => {
    expect(binaryFileName('ffmpeg', 'win32')).toBe('ffmpeg.exe');
    expect(binaryFileName('ffprobe', 'win32')).toBe('ffprobe.exe');
    expect(binaryFileName('ffmpeg', 'darwin')).toBe('ffmpeg');
    expect(binaryFileName('ffmpeg', 'linux')).toBe('ffmpeg');
  });

  it('does not double the suffix', () => {
    expect(binaryFileName('ffmpeg.exe', 'win32')).toBe('ffmpeg.exe');
  });
});

describe('chooseH264Encoder', () => {
  const listing = ' V....D h264_nvenc  NVIDIA\n V....D h264_qsv  Intel\n V....D h264_amf  AMD\n V....D mpeg4  MPEG-4\n';

  it('skips listed hardware encoders that fail the probe', () => {
    // the build lists nvenc/qsv/amf, but only AMF works on this machine
    const picked = chooseH264Encoder(listing, 'win32', (enc) => enc === 'h264_amf');
    expect(picked).toBe('h264_amf');
  });

  it('prefers the first working encoder in platform order', () => {
    expect(chooseH264Encoder(listing, 'win32', () => true)).toBe('h264_nvenc');
  });

  it('never probes encoders that are not compiled in', () => {
    const probed: string[] = [];
    chooseH264Encoder(' V....D mpeg4  MPEG-4\n', 'win32', (enc) => (probed.push(enc), true));
    expect(probed).toEqual(['mpeg4']);
  });

  it('falls back to mpeg4 when nothing works', () => {
    expect(chooseH264Encoder(listing, 'win32', () => false)).toBe('mpeg4');
  });
});
