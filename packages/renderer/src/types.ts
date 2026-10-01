/**
 * The Renderer interface is the seam between the editor and everything visual.
 * The canvas compositor below is the default implementation; if a Remotion adapter is
 * ever added it plugs in here (desktop decision #4: custom compositor ships first).
 */
import { Item, TimelineDoc } from '@cutboard/schema';

export type DrawableSource =
  | HTMLVideoElement
  | HTMLImageElement
  | ImageBitmap
  | HTMLCanvasElement
  | OffscreenCanvas;

export function drawableSize(src: DrawableSource): { width: number; height: number } {
  if (typeof HTMLVideoElement !== 'undefined' && src instanceof HTMLVideoElement) {
    return { width: src.videoWidth, height: src.videoHeight };
  }
  if (typeof HTMLImageElement !== 'undefined' && src instanceof HTMLImageElement) {
    return { width: src.naturalWidth, height: src.naturalHeight };
  }
  return { width: (src as ImageBitmap).width, height: (src as ImageBitmap).height };
}

export interface MediaResolver {
  /**
   * Return a drawable source for the asset, already seeked to `sourceFrame`.
   * Implementations should cache elements per asset and resolve seeks asynchronously.
   * Throw (or return null) when the media is missing — the compositor draws a placeholder.
   */
  getVisual(assetId: string, sourceFrame: number, item: Item): Promise<DrawableSource | null>;
  /** Resolve the media URL used by a resolver (cbmedia:// in the desktop app). */
  resolveMediaUrl(assetId: string): string | null;
  /**
   * Evaluate one frame of motion-graphic code in the sandbox (main prompt §7).
   * Returns the scene tree, or throws with the agent-facing error for the repair loop.
   */
  evaluateMotion?(code: string, props: Record<string, unknown>, frame: number, videoConfig: { width: number; height: number; fps: number; durationInFrames: number }): Promise<unknown>;
}

export interface DrawOptions {
  /** item ids to outline (UI selection) */
  selectedIds?: string[];
  showSafeZones?: boolean;
  /** draw a checkerboard instead of the project background (transparency view) */
  transparent?: boolean;
}

export interface Renderer {
  readonly kind: 'canvas-compositor';
  draw(doc: TimelineDoc, frame: number, resolver: MediaResolver, opts?: DrawOptions): Promise<void>;
  dispose(): void;
}
