import { Asset, TimelineDoc } from '@cutboard/schema';
import { DrawableSource, MediaResolver, drawableSize } from '@cutboard/renderer';
import { mediaUrl } from './url.ts';

/**
 * Preview media stack: an element pool per asset + a MediaResolver that seeks proxies
 * to exact source frames. Playback (play/pause) is orchestrated by PreviewPane; the
 * resolver handles the paused/scrub path where every frame must be exact.
 */

export function seekTo(el: HTMLVideoElement | HTMLAudioElement, seconds: number): Promise<void> {
  if (!Number.isFinite(seconds)) return Promise.resolve();
  const target = Math.max(0, seconds);
  // a frame is drawable once the element isn't mid-seek and has data for the current position
  const ready = () => !el.seeking && el.readyState >= 2;
  if (Math.abs(el.currentTime - target) < 0.004 && ready()) return Promise.resolve();
  return new Promise((resolve) => {
    // After a seek completes Chromium can still hand drawImage the previously presented
    // frame for a moment (reliably so in the hidden export/still windows), so for video also
    // wait for requestVideoFrameCallback. It is registered before the seek starts so a fast
    // seek can't present the frame before we are listening; if no new frame comes (the target
    // maps to the frame already shown) a short grace period ends the wait.
    const video = el as HTMLVideoElement & { requestVideoFrameCallback?: (cb: () => void) => number; cancelVideoFrameCallback?: (id: number) => void };
    const watchFrames = typeof video.requestVideoFrameCallback === 'function';
    let framed = !watchFrames;
    const frameCb = watchFrames ? video.requestVideoFrameCallback!(() => { framed = true; check(); }) : 0;
    let readyAt = 0;
    let settled = false;
    const done = () => {
      if (settled) return;
      settled = true;
      clearInterval(poll);
      clearTimeout(cap);
      el.removeEventListener('seeked', check);
      if (watchFrames && !framed) video.cancelVideoFrameCallback?.(frameCb);
      resolve();
    };
    // currentTime reports the target as soon as it is assigned, before anything is decoded, so
    // readiness comes from the seeking flag + readyState. Events are unreliable in hidden
    // windows, hence the poll; the cap only guards against a seek that never finishes.
    function check(): void {
      if (!ready()) return;
      if (!readyAt) readyAt = performance.now();
      if (framed || performance.now() - readyAt > 120) done();
    }
    const poll = setInterval(check, 8);
    const cap = setTimeout(done, 3000);
    el.addEventListener('seeked', check);
    el.currentTime = target;
  });
}

/** Seek-only resolver used by both the preview (paused) and the export window. */
export class MediaPool implements MediaResolver {
  private videos = new Map<string, HTMLVideoElement>();
  private images = new Map<string, HTMLImageElement>();
  private audio = new Map<string, HTMLAudioElement>();
  private pending = new Map<string, Promise<DrawableSource | null>>();
  private loadedOnce = new Set<string>();
  /** During realtime playback, playing video elements are drawn as-is (no seeks). */
  liveMode = false;

  /** All audio elements (for pause-all on stop). */
  get allAudio(): HTMLAudioElement[] {
    return [...this.audio.values()];
  }

  /** All video elements (for pause-all on stop). */
  get allVideoElements(): HTMLVideoElement[] {
    return [...this.videos.values()];
  }

  /** Video elements keyed by asset id (playback control in PreviewPane). */
  get videoElements(): Map<string, HTMLVideoElement> {
    return this.videos;
  }

  /** Motion-graphic sandbox host (lazy; created on first use). */
  private motionHost: import('./motion-host.ts').MotionHost | null = null;

  async evaluateMotion(
    code: string,
    props: Record<string, unknown>,
    frame: number,
    videoConfig: { width: number; height: number; fps: number; durationInFrames: number },
  ): Promise<unknown> {
    if (!this.motionHost) {
      const { MotionHost } = await import('./motion-host.ts');
      this.motionHost = new MotionHost();
    }
    return this.motionHost.evaluate(code, props, frame, videoConfig);
  }

  constructor(
    private readonly assets: Map<string, { proxyPath?: string; path: string; kind: string; fps?: number }>,
    private readonly projectFps: number,
  ) {}

  resolveMediaUrl(assetId: string): string | null {
    const asset = this.assets.get(assetId);
    if (!asset) return null;
    return mediaUrl(asset.proxyPath ?? asset.path);
  }

  /** Keep the pool in sync as assets are imported/analyzed (proxyPath appears later). */
  updateAssets(assets: Asset[]): void {
    for (const a of assets) {
      this.assets.set(a.id, { proxyPath: a.proxyPath, path: a.path, kind: a.kind, fps: a.fps });
    }
  }

  private ensureVideo(assetId: string): HTMLVideoElement | null {
    let el = this.videos.get(assetId);
    if (el) return el;
    const url = this.resolveMediaUrl(assetId);
    if (!url) return null;
    el = document.createElement('video');
    el.src = url;
    el.muted = true; // preview audio is handled by the audio pool
    el.preload = 'auto';
    el.playsInline = true;
    el.crossOrigin = 'anonymous';
    this.videos.set(assetId, el);
    return el;
  }

  private ensureImage(assetId: string): HTMLImageElement | null {
    let el = this.images.get(assetId);
    if (el) return el;
    const url = this.resolveMediaUrl(assetId);
    if (!url) return null;
    el = new Image();
    el.src = url;
    this.images.set(assetId, el);
    return el;
  }

  ensureAudio(assetId: string): HTMLAudioElement | null {
    let el = this.audio.get(assetId);
    if (el) return el;
    const asset = this.assets.get(assetId);
    if (!asset) return null;
    el = document.createElement('audio');
    el.src = mediaUrl(asset.path); // audio always from the original for quality
    el.preload = 'auto';
    el.crossOrigin = 'anonymous';
    this.audio.set(assetId, el);
    return el;
  }

  getAudioElement(assetId: string): HTMLAudioElement | null {
    return this.ensureAudio(assetId);
  }

  async getVisual(assetId: string, sourceFrame: number, _item?: unknown): Promise<DrawableSource | null> {
    const asset = this.assets.get(assetId);
    if (!asset) return null;
    const key = `${assetId}@${sourceFrame}`;
    const inflight = this.pending.get(key);
    if (inflight) return inflight;

    const promise = (async () => {
      if (asset.kind === 'image') {
        const el = this.ensureImage(assetId);
        if (!el) return null;
        if (!el.complete) {
          await new Promise<void>((resolve) => {
            const poll = setInterval(() => {
              if (el.complete) {
                clearInterval(poll);
                clearTimeout(cap);
                resolve();
              }
            }, 16);
            const cap = setTimeout(() => {
              clearInterval(poll);
              resolve();
            }, 3000);
          });
        }
        return el.complete && el.naturalWidth > 0 ? el : null;
      }
      const el = this.ensureVideo(assetId);
      if (!el) return null;
      if (this.liveMode && !el.paused && el.readyState >= 2) {
        return el.videoWidth > 0 ? el : null; // already playing — draw the live frame
      }
      // One-time load wait per element; afterwards, seeking keeps readyState bouncing
      // (HAVE_METADATA during seeks) so we never gate per-frame on it.
      if (!this.loadedOnce.has(assetId) && (el.readyState < 2 || el.videoWidth === 0)) {
        await new Promise<void>((resolve) => {
          const poll = setInterval(() => {
            if (el.readyState >= 2 && el.videoWidth > 0) {
              clearInterval(poll);
              clearTimeout(cap);
              resolve();
            }
          }, 16);
          const cap = setTimeout(() => {
            clearInterval(poll);
            resolve();
          }, 6000);
        });
      }
      this.loadedOnce.add(assetId);
      // source frames are in project-fps timebase (docs/DECISIONS.md)
      const seconds = sourceFrame / this.projectFps;
      await seekTo(el, seconds);
      if (el.videoWidth === 0) return null;
      return el;
    })();

    this.pending.set(key, promise);
    try {
      return await promise;
    } finally {
      this.pending.delete(key);
    }
  }

  /** Best-effort intrinsic size for an asset (used by drag-drop). */
  sizeOf(assetId: string): { width: number; height: number } | null {
    const asset = this.assets.get(assetId);
    if (!asset) return null;
    const el = asset.kind === 'image' ? this.images.get(assetId) : this.videos.get(assetId);
    if (!el) return null;
    const size = drawableSize(el as DrawableSource);
    return size.width > 0 ? size : null;
  }

  dispose(): void {
    for (const el of this.videos.values()) {
      el.pause();
      el.removeAttribute('src');
      el.load();
    }
    for (const el of this.audio.values()) {
      el.pause();
      el.removeAttribute('src');
      el.load();
    }
    this.videos.clear();
    this.images.clear();
    this.audio.clear();
  }
}

export interface AssetMediaInfo {
  proxyPath?: string;
  path: string;
  kind: string;
  fps?: number;
}

export function assetsToMap(assets: Asset[]): Map<string, AssetMediaInfo> {
  return new Map(assets.map((a) => [a.id, { proxyPath: a.proxyPath, path: a.path, kind: a.kind, fps: a.fps }]));
}

export type { TimelineDoc };
