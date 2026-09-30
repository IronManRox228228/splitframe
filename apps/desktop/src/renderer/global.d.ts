import type { CutboardApi } from '../preload/index.ts';

declare global {
  interface Window {
    cutboard: CutboardApi;
  }
}

export {};
