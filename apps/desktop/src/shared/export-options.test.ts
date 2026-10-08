import { describe, expect, it } from 'vitest';
import { estimateBytes, findPreset, listPresets, formatBytes, resolveExport, sanitizeFileName } from './export-options.ts';

describe('resolveExport', () => {
  it('matches the platform presets at 1080p', () => {
    expect(resolveExport('1080p', 'mp4', 1920, 1080)).toMatchObject({ width: 1920, height: 1080, videoBitrateK: 12000 });
    expect(resolveExport('1080p', 'mp4', 1080, 1920)).toMatchObject({ width: 1080, height: 1920 });
    expect(resolveExport('1080p', 'mp4', 1080, 1080)).toMatchObject({ width: 1080, height: 1080 });
  });
  it('scales by the short side and keeps even dimensions', () => {
    const r = resolveExport('720p', 'webm', 1080, 1350);
    expect(r.width).toBe(720);
    expect(r.height).toBe(900);
    expect(r.width % 2).toBe(0);
    expect(r.height % 2).toBe(0);
    expect(r.format).toBe('webm');
  });
});

describe('estimates and names', () => {
  it('estimates size from bitrate and duration', () => {
    expect(estimateBytes(12000, 10)).toBe(Math.round((12128 * 1000 * 10) / 8));
    expect(formatBytes(18 * 1024 * 1024)).toBe('18 MB');
  });
  it('sanitizes file names', () => {
    expect(sanitizeFileName('a/b\\c:d*e?"f<g>h|i')).toBe('abcdefghi');
    expect(sanitizeFileName('  trailing dots... ')).toBe('trailing dots');
    expect(sanitizeFileName('CON')).toBe('_CON');
    expect(sanitizeFileName('..hidden')).toBe('hidden');
    expect(sanitizeFileName('x'.repeat(300)).length).toBe(120);
  });
});

describe('agent-visible presets', () => {
  it('lists a 720p tier resolved against the canvas, same as the dialog', () => {
    const names = listPresets(1920, 1080).map((p) => p.name);
    expect(names).toContain('720p MP4');
    expect(names).toContain('YouTube 1080p');
    expect(findPreset('720p', 1920, 1080)).toMatchObject({ width: 1280, height: 720, format: 'mp4' });
    expect(findPreset('720P WEBM', 1920, 1080)).toMatchObject({ height: 720, format: 'webm' });
    expect(findPreset('720p', 1080, 1920)).toMatchObject({ width: 720, height: 1280 });
    expect(findPreset('nope', 1920, 1080)).toBeUndefined();
  });
});
