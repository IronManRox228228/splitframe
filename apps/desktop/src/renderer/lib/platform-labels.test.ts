import { describe, expect, it } from 'vitest';
import { deleteKeyLabel, revealLabel, shortcutLabel } from './platform-labels.ts';

describe('platform labels', () => {
  it('uses the command key only on macOS', () => {
    expect(shortcutLabel('D', 'darwin')).toBe('⌘D');
    expect(shortcutLabel('D', 'win32')).toBe('Ctrl+D');
    expect(shortcutLabel('D', undefined)).toBe('Ctrl+D');
    expect(deleteKeyLabel('darwin')).toBe('⌫');
    expect(deleteKeyLabel('linux')).toBe('Del');
  });

  it('names the right file manager', () => {
    expect(revealLabel('darwin')).toBe('Show in Finder');
    expect(revealLabel('win32')).toBe('Show in Explorer');
    expect(revealLabel('linux')).toBe('Show in folder');
  });
});
