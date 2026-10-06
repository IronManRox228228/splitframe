import { describe, expect, it } from 'vitest';
import { shortcutFor, type KeyInfo } from './shortcuts.ts';

const key = (over: Partial<KeyInfo>): KeyInfo => ({
  key: '',
  code: '',
  metaKey: false,
  ctrlKey: false,
  shiftKey: false,
  targetTag: 'BODY',
  isContentEditable: false,
  ...over,
});
const idle = { playing: false };

describe('shortcutFor', () => {
  it('treats Ctrl and Cmd alike for undo/redo/clone/import', () => {
    expect(shortcutFor(key({ key: 'z', ctrlKey: true }), idle)).toBe('undo');
    expect(shortcutFor(key({ key: 'z', metaKey: true }), idle)).toBe('undo');
    expect(shortcutFor(key({ key: 'Z', ctrlKey: true, shiftKey: true }), idle)).toBe('redo');
    expect(shortcutFor(key({ key: 'd', ctrlKey: true }), idle)).toBe('clone');
    expect(shortcutFor(key({ key: 'i', metaKey: true }), idle)).toBe('import');
  });

  it('leaves Ctrl/Cmd zoom to the menu so it does not zoom twice', () => {
    expect(shortcutFor(key({ key: '=', ctrlKey: true }), idle)).toBeNull();
    expect(shortcutFor(key({ key: '-', metaKey: true }), idle)).toBeNull();
    expect(shortcutFor(key({ key: '=' }), idle)).toBe('zoomIn');
    expect(shortcutFor(key({ key: '-' }), idle)).toBe('zoomOut');
  });

  it('does not turn modified J/K/L/S into transport or split shortcuts', () => {
    for (const k of ['j', 'k', 'l', 's']) expect(shortcutFor(key({ key: k, ctrlKey: true }), idle)).toBeNull();
  });

  it('ignores every key while a text field, select or editable element has focus', () => {
    for (const tag of ['INPUT', 'TEXTAREA', 'SELECT']) {
      expect(shortcutFor(key({ key: 'ArrowLeft', targetTag: tag }), idle)).toBeNull();
      expect(shortcutFor(key({ key: 'Backspace', targetTag: tag }), idle)).toBeNull();
    }
    expect(shortcutFor(key({ key: 's', isContentEditable: true }), idle)).toBeNull();
  });

  it('maps transport keys using the playing state', () => {
    expect(shortcutFor(key({ code: 'Space', key: ' ' }), idle)).toBe('togglePlay');
    expect(shortcutFor(key({ key: 'l' }), { playing: false })).toBe('play');
    expect(shortcutFor(key({ key: 'l' }), { playing: true })).toBeNull();
    expect(shortcutFor(key({ key: 'k' }), { playing: true })).toBe('pause');
    expect(shortcutFor(key({ key: 'Z', shiftKey: true }), idle)).toBe('zoomFit');
  });
});
