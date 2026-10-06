import { CanvasCompositor, MOTION_RUNTIME_SOURCE, MOTION_EVAL_SOURCE } from '@cutboard/renderer';
import { docDurationFrames } from '@cutboard/editor-core';
import type { TimelineDoc } from '@cutboard/schema';
import { MediaPool } from './lib/media.ts';

// motion-graphics sandbox runtime (provisioned before any frame renders)
(window as unknown as Record<string, unknown>)['__cutboardMotionRuntime'] = MOTION_RUNTIME_SOURCE + MOTION_EVAL_SOURCE;

/**
 * Export window script: renders the timeline frame-by-frame with the same compositor as
 * the preview (preview == output, main prompt §9) and streams raw RGBA to the main
 * process, which pipes it into ffmpeg. Audio is mixed by ffmpeg separately.
 */

interface ExportSettings {
  exportId: string;
  width: number;
  height: number;
}

async function main(): Promise<void> {
  const params = new URLSearchParams(window.location.search);
  const exportId = params.get('exportId');
  const width = Number(params.get('width') ?? 1920);
  const height = Number(params.get('height') ?? 1080);
  const fps = Number(params.get('fps') ?? 30);
  const stillFrame = params.get('stillFrame'); // single-frame mode for captureFrame
  if (!exportId) {
    window.cutboard.sendExportError('', 'Missing exportId');
    return;
  }
  const settings: ExportSettings = { exportId, width, height };

  try {
    const bundle = (await window.cutboard.exportBundle(exportId)) as {
      doc: TimelineDoc;
      mediaUrls: Record<string, string>;
    };
    const doc = bundle.doc;
    const totalFrames = docDurationFrames(doc);
    if (totalFrames === 0) throw new Error('Nothing to render');

    // main already chose the best file per asset (the original when Chromium can decode it)
    const assets = Object.entries(bundle.mediaUrls).map(([id, path]) => ({
      id,
      path,
      kind: doc.items.find((i) => i.assetId === id)?.type === 'image' ? 'image' : 'video',
    }));
    const pool = new MediaPool(new Map(assets.map((a) => [a.id, { path: a.path, kind: a.kind }])), fps);
    const compositor = new CanvasCompositor();
    const canvas = new OffscreenCanvas(settings.width, settings.height);
    const ctx = canvas.getContext('2d')!;

    if (stillFrame !== null) {
      // captureFrame mode: render one frame, send a PNG, exit
      const frame = Math.max(0, Math.min(totalFrames - 1, Number(stillFrame) || 0));
      await compositor.draw(canvas, doc, frame, pool, {});
      const blob = await canvas.convertToBlob({ type: 'image/png' });
      const buf = await blob.arrayBuffer();
      window.cutboard.sendExportFrame(settings.exportId, 0, buf, settings.width, settings.height);
      window.cutboard.sendExportDone(settings.exportId);
      return;
    }

    // group frames by which assets they need so we seek each video at most once per cut
    const maxFrames = Number(params.get('maxFrames') ?? totalFrames);
    const frameLimit = Math.min(totalFrames, Number.isFinite(maxFrames) && maxFrames > 0 ? maxFrames : totalFrames);
    for (let frame = 0; frame < frameLimit; frame++) {
      const t0 = performance.now();
      await compositor.draw(canvas, doc, frame, pool, {});
      const image = ctx.getImageData(0, 0, settings.width, settings.height);
      // copy out the underlying buffer (getImageData's buffer is reusable)
      const copy = new Uint8ClampedArray(image.data);
      window.cutboard.sendExportFrame(settings.exportId, frame, copy.buffer, settings.width, settings.height);
      if (frame % 10 === 0 || frame === frameLimit - 1) {
        console.log(`[export] frame ${frame + 1}/${frameLimit} took ${Math.round(performance.now() - t0)}ms`);
      }
    }
    window.cutboard.sendExportDone(settings.exportId);
  } catch (err) {
    window.cutboard.sendExportError(exportId, err instanceof Error ? err.message : String(err));
  }
}

void main();
