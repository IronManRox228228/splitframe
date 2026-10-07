import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { classifyImportPaths, isMediaFile, isUnder, removableGeneratedFiles } from './import-paths.ts';

let dir: string;
beforeAll(() => {
  dir = mkdtempSync(join(tmpdir(), 'sf-import-'));
  writeFileSync(join(dir, 'clip.MP4'), 'x');
  writeFileSync(join(dir, 'song.wav'), 'x');
  writeFileSync(join(dir, 'notes.txt'), 'x');
  mkdirSync(join(dir, 'folder.mp4'));
});
afterAll(() => rmSync(dir, { recursive: true, force: true }));

describe('isMediaFile', () => {
  it('matches supported extensions case-insensitively', () => {
    expect(isMediaFile('a/b/clip.MP4')).toBe(true);
    expect(isMediaFile('photo.jpeg')).toBe(true);
    expect(isMediaFile('notes.txt')).toBe(false);
    expect(isMediaFile('mp4')).toBe(false);
    expect(isMediaFile('.mp4')).toBe(false);
  });
});

describe('classifyImportPaths', () => {
  it('accepts real media files and skips everything else by name', () => {
    const res = classifyImportPaths([
      join(dir, 'clip.MP4'),
      join(dir, 'song.wav'),
      join(dir, 'notes.txt'),
      join(dir, 'folder.mp4'), // a directory with a media extension
      join(dir, 'missing.mov'),
      'relative/clip.mp4',
    ]);
    expect(res.accepted).toEqual([join(dir, 'clip.MP4'), join(dir, 'song.wav')]);
    expect(res.skipped).toEqual(['notes.txt', 'folder.mp4', 'missing.mov', 'clip.mp4']);
  });

  it('deduplicates repeated paths and rejects NUL bytes', () => {
    const p = join(dir, 'clip.MP4');
    expect(classifyImportPaths([p, p]).accepted).toEqual([p]);
    expect(classifyImportPaths([`${p}\0.txt`]).accepted).toEqual([]);
  });
});

describe('removableGeneratedFiles', () => {
  const projectDir = join(dir ?? tmpdir(), 'proj');
  it('only returns generated files inside the project folder, never the original', () => {
    const original = join(tmpdir(), 'Videos', 'holiday.mp4');
    const files = removableGeneratedFiles(
      {
        path: original,
        proxyPath: join(projectDir, 'cache', 'a-proxy.mp4'),
        thumbPath: original, // images use the original as their thumbnail
        waveformPath: join(tmpdir(), 'elsewhere', 'wave.png'),
      },
      [join(projectDir, 'cache', 'kf0.jpg'), join(projectDir, '..', 'other-project', 'cache', 'kf.jpg')],
      projectDir,
    );
    expect(files).toEqual([join(projectDir, 'cache', 'a-proxy.mp4'), join(projectDir, 'cache', 'kf0.jpg')]);
  });

  it('isUnder rejects the root itself and parent escapes', () => {
    expect(isUnder(projectDir, projectDir)).toBe(false);
    expect(isUnder(projectDir, join(projectDir, '..', 'x'))).toBe(false);
    expect(isUnder(projectDir, join(projectDir, 'cache', 'x'))).toBe(true);
  });
});
