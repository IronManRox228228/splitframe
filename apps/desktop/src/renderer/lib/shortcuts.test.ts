import { describe, expect, it } from 'vitest';
import { clipEdges, neighborEdge, runShortcut, shortcutFor, type KeyInfo, type ShortcutStore } from './shortcuts.ts';

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

describe('new shortcuts', () => {
  it('maps navigation, selection and snap keys', () => {
    expect(shortcutFor(key({ key: 'Home' }), idle)).toBe('goStart');
    expect(shortcutFor(key({ key: 'End' }), idle)).toBe('goEnd');
    expect(shortcutFor(key({ key: 'ArrowUp' }), idle)).toBe('prevEdge');
    expect(shortcutFor(key({ key: 'ArrowDown' }), idle)).toBe('nextEdge');
    expect(shortcutFor(key({ key: 'a', ctrlKey: true }), idle)).toBe('selectAll');
    expect(shortcutFor(key({ key: 'a', metaKey: true }), idle)).toBe('selectAll');
    expect(shortcutFor(key({ key: 'n' }), idle)).toBe('toggleSnap');
    expect(shortcutFor(key({ key: 'ArrowLeft', shiftKey: true }), idle)).toBe('secondBack');
    expect(shortcutFor(key({ key: 'ArrowRight', shiftKey: true }), idle)).toBe('secondForward');
    expect(shortcutFor(key({ key: 'j' }), idle)).toBe('secondBack');
    expect(shortcutFor(key({ key: 'a', ctrlKey: true, targetTag: 'INPUT' }), idle)).toBeNull();
  });
});

describe('edges', () => {
  const edges = clipEdges([
    { startFrame: 0, durationFrames: 30 },
    { startFrame: 30, durationFrames: 20 },
    { startFrame: 80, durationFrames: 10 },
  ]);
  it('dedupes and sorts', () => expect(edges).toEqual([0, 30, 50, 80, 90]));
  it('finds neighbours', () => {
    expect(neighborEdge(edges, 30, 1)).toBe(50);
    expect(neighborEdge(edges, 30, -1)).toBe(0);
    expect(neighborEdge(edges, 90, 1)).toBeNull();
    expect(neighborEdge(edges, 0, -1)).toBeNull();
  });
});

describe('runShortcut', () => {
  const make = (fps: number): ShortcutStore & { calls: string[] } => {
    const calls: string[] = [];
    const noop = (name: string) => () => {
      calls.push(name);
    };
    return {
      calls,
      doc: { project: { fps }, items: [{ id: 'a', startFrame: 0, durationFrames: 100 }] },
      playhead: 10,
      playing: false,
      pxPerFrame: 2,
      togglePlay: noop('togglePlay'),
      setPlayhead: (f) => calls.push(`seek:${f}`),
      stepFrames: (n) => calls.push(`step:${n}`),
      undo: async () => {},
      redo: async () => {},
      cloneSelection: async () => {},
      importMedia: async () => {},
      deleteSelection: async () => {},
      splitAtPlayhead: async () => {},
      setPxPerFrame: (px) => calls.push(`px:${px}`),
      zoomFit: (n) => calls.push(`fit:${n}`),
      toggleSnap: noop('snap'),
      select: noop('select'),
      setSelection: (ids) => calls.push(`sel:${ids.join(',')}`),
    };
  };
  it('steps one second at the project fps', () => {
    const s = make(24);
    runShortcut('secondForward', s);
    runShortcut('secondBack', s);
    expect(s.calls).toEqual(['step:24', 'step:-24']);
  });
  it('goes to start and end', () => {
    const s = make(30);
    runShortcut('goStart', s);
    runShortcut('goEnd', s);
    expect(s.calls).toEqual(['seek:0', 'seek:100']);
  });
  it('selects all items', () => {
    const s = make(30);
    runShortcut('selectAll', s);
    expect(s.calls).toEqual(['sel:a']);
  });
});
