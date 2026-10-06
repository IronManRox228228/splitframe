import { afterEach, describe, expect, it, vi } from 'vitest';
import { seekTo } from './media.ts';

/** Minimal stand-in for a media element: assigning currentTime starts a seek that
 *  finishes (seeking=false, data decoded) only after `decodeMs`, like a real element. */
function fakeMedia(decodeMs: number) {
  const listeners = new Map<string, () => void>();
  const el = {
    seeking: false,
    readyState: 4,
    _t: 0,
    get currentTime() { return this._t; },
    set currentTime(v: number) {
      this._t = v; // spec: currentTime reports the new position immediately
      this.seeking = true;
      this.readyState = 1;
      setTimeout(() => { this.seeking = false; this.readyState = 4; listeners.get('seeked')?.(); }, decodeMs);
    },
    addEventListener: (type: string, fn: () => void) => listeners.set(type, fn),
    removeEventListener: (type: string) => listeners.delete(type),
  };
  return el;
}

afterEach(() => vi.useRealTimers());

describe('seekTo', () => {
  it('waits for the frame to be decoded, not just for currentTime to change', async () => {
    vi.useFakeTimers();
    const el = fakeMedia(700);
    let resolved = false;
    void seekTo(el as unknown as HTMLVideoElement, 5).then(() => { resolved = true; });
    await vi.advanceTimersByTimeAsync(100);
    expect(resolved).toBe(false); // currentTime already reads 5, but the frame isn't there yet
    await vi.advanceTimersByTimeAsync(700);
    expect(resolved).toBe(true);
    expect(el.seeking).toBe(false);
  });

  it('still gives up eventually if a seek never completes', async () => {
    vi.useFakeTimers();
    const el = fakeMedia(60_000);
    let resolved = false;
    void seekTo(el as unknown as HTMLVideoElement, 5).then(() => { resolved = true; });
    await vi.advanceTimersByTimeAsync(5000);
    expect(resolved).toBe(true);
  });
});
