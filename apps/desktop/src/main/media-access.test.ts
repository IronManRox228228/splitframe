import { describe, expect, it } from 'vitest';
import { isInsideRoot, isMediaPathAllowed } from './media-access.ts';

describe('isInsideRoot', () => {
  it('accepts files under the root and rejects siblings that merely share a prefix', () => {
    expect(isInsideRoot('/home/u/Videos/Cutboard/p1/cache/a.jpg', '/home/u/Videos/Cutboard', 'linux')).toBe(true);
    expect(isInsideRoot('/home/u/Videos/Cutboard-evil/a.jpg', '/home/u/Videos/Cutboard', 'linux')).toBe(false);
  });

  it('does not let .. segments climb out of the root', () => {
    expect(isInsideRoot('/home/u/Videos/Cutboard/../../.ssh/id_rsa', '/home/u/Videos/Cutboard', 'linux')).toBe(false);
  });

  it('compares Windows paths case-insensitively', () => {
    expect(isInsideRoot('c:\\users\\u\\videos\\cutboard\\p1\\x.png', 'C:\\Users\\U\\Videos\\Cutboard', 'win32')).toBe(true);
  });

  it('rejects relative paths', () => {
    expect(isInsideRoot('cache/a.jpg', '/home/u/Videos/Cutboard', 'linux')).toBe(false);
  });
});

describe('isMediaPathAllowed', () => {
  const roots = ['/home/u/Videos/Cutboard'];
  const known = new Set(['/mnt/footage/clip.mp4']);
  const allow = (p: string) => isMediaPathAllowed(p, { roots, isKnownAssetPath: (x) => known.has(x), platform: 'linux' });

  it('serves project files and registered assets', () => {
    expect(allow('/home/u/Videos/Cutboard/p1/cache/ast-proxy.mp4')).toBe(true);
    expect(allow('/mnt/footage/clip.mp4')).toBe(true);
  });

  it('refuses any other file on disk', () => {
    expect(allow('/home/u/.ssh/id_rsa')).toBe(false);
    expect(allow('/etc/passwd')).toBe(false);
    expect(allow('relative/path.mp4')).toBe(false);
  });
});
