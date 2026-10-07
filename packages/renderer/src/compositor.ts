import { CaptionStyle, Item, TimelineDoc, TextStyle } from '@cutboard/schema';
import type { MotionNode } from './motion.ts';
import { itemEnd, sourceFrameAt, itemsOnTrack } from '@cutboard/editor-core';
import { effectsToFilter } from './effects.ts';
import { captionCardAt, captionClockMs } from './captions.ts';
import { itemOpacityAt, resolveItemProperty } from './keyframes.ts';
import { DrawableSource, DrawOptions, MediaResolver, drawableSize } from './types.ts';

type Ctx2D = CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;

/**
 * The default compositor: one canvas, drawn bottom-up from the track list
 * (tracks[0] is the top layer). Preview and export use the exact same code path so
 * output matches what the editor shows (main prompt §9).
 */
export class CanvasCompositor {
  readonly kind = 'canvas-compositor' as const;

  async draw(
    canvas: HTMLCanvasElement | OffscreenCanvas,
    doc: TimelineDoc,
    frame: number,
    resolver: MediaResolver,
    opts: DrawOptions = {},
  ): Promise<void> {
    const ctx = canvas.getContext('2d') as Ctx2D | null;
    if (!ctx) throw new Error('Canvas 2D context unavailable');
    const { width, height } = canvas;
    ctx.clearRect(0, 0, width, height);
    if (opts.transparent) {
      drawCheckerboard(ctx, width, height);
    } else {
      ctx.fillStyle = doc.project.styleConfig?.backgroundColor ?? '#000000';
      ctx.fillRect(0, 0, width, height);
    }

    const active = activeItemsByDrawOrder(doc, frame);
    for (const item of active) {
      await drawItem(ctx, doc, item, frame, resolver, width, height);
    }

    if (opts.selectedIds && opts.selectedIds.length > 0) {
      for (const item of active) {
        if (opts.selectedIds.includes(item.id)) {
          drawSelectionOutline(ctx, doc, item, frame, width, height);
        }
      }
    }

    if (opts.showSafeZones) drawSafeZones(ctx, width, height);
  }

  dispose(): void {
    /* stateless */
  }
}

export function activeItemsByDrawOrder(doc: TimelineDoc, frame: number): Item[] {
  const out: Item[] = [];
  // bottom layer first: reverse of the top-first track array
  for (let t = doc.tracks.length - 1; t >= 0; t--) {
    const track = doc.tracks[t]!;
    if (track.hidden) continue;
    for (const item of itemsOnTrack(doc, track.id)) {
      if (frame >= item.startFrame && frame < itemEnd(item)) out.push(item);
    }
  }
  return out;
}

async function drawItem(
  ctx: Ctx2D,
  doc: TimelineDoc,
  item: Item,
  frame: number,
  resolver: MediaResolver,
  canvasW: number,
  canvasH: number,
): Promise<void> {
  const t = item.transform;
  const cx = canvasW / 2 + resolveItemProperty(item, 'transform.x', frame);
  const cy = canvasH / 2 + resolveItemProperty(item, 'transform.y', frame);
  const scale = resolveItemProperty(item, 'transform.scale', frame);
  const scaleX = t.scaleX;
  const scaleY = t.scaleY;
  const rotation = resolveItemProperty(item, 'transform.rotation', frame);
  const alpha = itemOpacityAt(item, frame);
  if (alpha <= 0) return;

  ctx.save();
  ctx.globalAlpha = alpha;
  try {
    if (item.masks.length > 0) applyMasks(ctx, item, canvasW, canvasH);

    if (item.type === 'video' || item.type === 'image') {
      const filter = effectsToFilter(item.effects);
      if (filter !== 'none') ctx.filter = filter;
      const sourceFrame = sourceFrameAt(item, frame);
      const media = await safeResolve(resolver, item.assetId!, sourceFrame, item);
      if (media) {
        drawFitted(ctx, media, cx, cy, scale * scaleX, scale * scaleY, rotation);
      } else {
        drawMissingPlaceholder(ctx, item, cx, cy, scale, rotation, canvasW, canvasH);
      }
      ctx.filter = 'none';
    } else if (item.type === 'text') {
      const props = item.props as { text: string; style: TextStyle };
      drawTextBlock(ctx, props.text, props.style, cx, cy, scale * scaleX, scale * scaleY, rotation);
    } else if (item.type === 'caption') {
      const props = item.props as {
        words: { w: string; startMs: number; endMs: number }[];
        style: CaptionStyle;
        maxWordsPerCard: number;
      };
      drawCaption(ctx, doc, item, props, frame, canvasW, canvasH, scale, rotation);
    } else if (item.type === 'shape') {
      drawShape(ctx, item, cx, cy, scale * scaleX, scale * scaleY, rotation);
    } else if (item.type === 'motionGraphic') {
      // Evaluate the generated composition in the sandbox and draw the scene tree;
      // preview and export share this exact path (main prompt §9).
      if (resolver.evaluateMotion) {
        try {
          const props = (item.props as { code: string; inputProps: Record<string, unknown> }).inputProps ?? {};
          const code = (item.props as { code: string }).code;
          const localFrame = frame - item.startFrame;
          const tree = await resolver.evaluateMotion(code, props, localFrame, {
            width: canvasW,
            height: canvasH,
            fps: doc.project.fps,
            durationInFrames: item.durationFrames,
          });
          ctx.save();
          ctx.translate(cx, cy);
          if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
          ctx.scale(scale, scale);
          await drawSceneTree(ctx, tree as MotionNode | null, resolver, canvasW, canvasH);
          ctx.restore();
        } catch (err) {
          drawMotionError(ctx, err instanceof Error ? err.message : String(err), cx, cy, canvasW, canvasH);
        }
      } else {
        drawMotionGraphicPlaceholder(ctx, item, cx, cy, scale, rotation);
      }
    }
  } finally {
    ctx.restore();
  }
}

async function safeResolve(
  resolver: MediaResolver,
  assetId: string,
  sourceFrame: number,
  item: Item,
): Promise<DrawableSource | null> {
  try {
    return await resolver.getVisual(assetId, sourceFrame, item);
  } catch {
    return null;
  }
}

/** Draw media fitted (contain) into the canvas, centered, with scale/rotation applied. */
function drawFitted(
  ctx: Ctx2D,
  media: DrawableSource,
  cx: number,
  cy: number,
  scaleX: number,
  scaleY: number,
  rotation: number,
): void {
  const size = drawableSize(media);
  if (size.width === 0 || size.height === 0) return;
  const canvasW = ctx.canvas.width;
  const canvasH = ctx.canvas.height;
  const fit = Math.min(canvasW / size.width, canvasH / size.height);
  const w = size.width * fit;
  const h = size.height * fit;
  ctx.save();
  ctx.translate(cx, cy);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  ctx.scale(scaleX, scaleY);
  ctx.drawImage(media as CanvasImageSource, -w / 2, -h / 2, w, h);
  ctx.restore();
}

function drawMissingPlaceholder(
  ctx: Ctx2D,
  item: Item,
  cx: number,
  cy: number,
  scale: number,
  rotation: number,
  canvasW: number,
  canvasH: number,
): void {
  ctx.save();
  ctx.translate(cx, cy);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  const w = canvasW * 0.6 * scale;
  const h = canvasH * 0.6 * scale;
  ctx.fillStyle = '#1f2937';
  ctx.fillRect(-w / 2, -h / 2, w, h);
  ctx.strokeStyle = '#6b7280';
  ctx.setLineDash([8, 6]);
  ctx.strokeRect(-w / 2, -h / 2, w, h);
  ctx.setLineDash([]);
  ctx.fillStyle = '#9ca3af';
  ctx.font = '20px system-ui, sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(`Media unavailable — ${item.labels?.name ?? item.id}`, 0, 0);
  ctx.restore();
}

function drawTextBlock(
  ctx: Ctx2D,
  text: string,
  style: TextStyle,
  cx: number,
  cy: number,
  scaleX: number,
  scaleY: number,
  rotation: number,
): void {
  ctx.save();
  ctx.translate(cx, cy);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  ctx.scale(scaleX, scaleY);
  ctx.font = `${style.fontWeight} ${style.fontSize}px ${style.fontFamily}, Geist, system-ui, sans-serif`;
  ctx.textAlign = style.align === 'center' ? 'center' : style.align;
  ctx.textBaseline = 'middle';
  const lines = text.split('\n');
  const lineHeight = style.fontSize * style.lineHeight;
  if (style.backgroundColor) {
    const widest = Math.max(...lines.map((l) => ctx.measureText(l).width));
    const pad = style.padding;
    ctx.fillStyle = style.backgroundColor;
    roundedRectPath(
      ctx,
      -widest / 2 - pad,
      (-lines.length * lineHeight) / 2 - pad * 0.5,
      widest + pad * 2,
      lines.length * lineHeight + pad,
      style.borderRadius,
    );
    ctx.fill();
  }
  ctx.lineWidth = style.strokeWidth;
  ctx.strokeStyle = style.strokeColor ?? 'transparent';
  lines.forEach((line, i) => {
    const y = (i - (lines.length - 1) / 2) * lineHeight;
    if (style.uppercase) line = line.toUpperCase();
    if (style.strokeWidth > 0) ctx.strokeText(line, 0, y);
    ctx.fillStyle = style.color;
    ctx.fillText(line, 0, y);
  });
  ctx.restore();
}

/** Captions: group words into cards of maxWordsPerCard, karaoke-highlight the active word. */
function drawCaption(
  ctx: Ctx2D,
  doc: TimelineDoc,
  item: Item,
  props: {
    words: { w: string; startMs: number; endMs: number }[];
    style: CaptionStyle;
    maxWordsPerCard: number;
  },
  frame: number,
  canvasW: number,
  canvasH: number,
  scale: number,
  rotation: number,
): void {
  const fps = doc.project.fps;
  const { words, style, maxWordsPerCard } = props;
  // word times are relative to the caption item's start
  const timeMs = captionClockMs(frame, item.startFrame, fps);
  const shown = captionCardAt(words, timeMs, maxWordsPerCard);
  if (!shown) return;
  const { card } = shown;

  ctx.save();
  ctx.translate(canvasW / 2 + item.transform.x, canvasH * style.placementY + item.transform.y);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  ctx.scale(scale, scale);
  ctx.font = `${style.fontWeight} ${style.fontSize}px ${style.fontFamily}, Geist, system-ui, sans-serif`;
  ctx.textBaseline = 'middle';

  // wrap into lines by maxCharsPerLine
  const lines: { w: string; startMs: number; endMs: number; active: boolean }[][] = [[]];
  let charCount = 0;
  for (const word of card) {
    const active = timeMs >= word.startMs && timeMs < word.endMs;
    const len = word.w.length + 1;
    if (charCount + len > style.maxCharsPerLine && lines[lines.length - 1]!.length > 0) {
      lines.push([]);
      charCount = 0;
    }
    lines[lines.length - 1]!.push({ ...word, active });
    charCount += len;
  }

  const lineHeight = style.fontSize * style.lineHeight * 1.15;
  const spaceW = ctx.measureText(' ').width;
  lines.forEach((lineWords, li) => {
    const y = (li - (lines.length - 1) / 2) * lineHeight;
    const totalWidth =
      lineWords.reduce((sum, w) => sum + ctx.measureText(w.w).width, 0) + spaceW * (lineWords.length - 1);
    let x = -totalWidth / 2;
    ctx.textAlign = 'left';
    for (const word of lineWords) {
      const wWidth = ctx.measureText(word.w).width;
      if (style.highlight === 'word-bg' && word.active) {
        ctx.fillStyle = style.highlightColor;
        roundedRectPath(ctx, x - 4, y - style.fontSize * 0.6, wWidth + 8, style.fontSize * 1.2, 6);
        ctx.fill();
      }
      if (style.strokeWidth > 0) {
        ctx.lineWidth = style.strokeWidth;
        ctx.strokeStyle = style.strokeColor ?? '#000000';
        ctx.strokeText(word.w, x, y);
      }
      ctx.fillStyle =
        style.highlight === 'active-word' && word.active ? style.highlightColor : style.color;
      ctx.fillText(word.w, x, y);
      x += wWidth + spaceW;
    }
  });
  ctx.restore();
}

function drawShape(
  ctx: Ctx2D,
  item: Item,
  cx: number,
  cy: number,
  scaleX: number,
  scaleY: number,
  rotation: number,
): void {
  const props = item.props as {
    shape: 'rect' | 'ellipse' | 'triangle';
    fill: string;
    stroke?: string;
    strokeWidth: number;
    radius: number;
    width: number;
    height: number;
  };
  ctx.save();
  ctx.translate(cx, cy);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  ctx.scale(scaleX, scaleY);
  const w = props.width;
  const h = props.height;
  ctx.beginPath();
  if (props.shape === 'rect') {
    roundedRectPath(ctx, -w / 2, -h / 2, w, h, props.radius);
  } else if (props.shape === 'ellipse') {
    ctx.ellipse(0, 0, w / 2, h / 2, 0, 0, Math.PI * 2);
  } else {
    ctx.moveTo(0, -h / 2);
    ctx.lineTo(w / 2, h / 2);
    ctx.lineTo(-w / 2, h / 2);
    ctx.closePath();
  }
  ctx.fillStyle = props.fill;
  ctx.fill();
  if (props.strokeWidth > 0 && props.stroke) {
    ctx.lineWidth = props.strokeWidth;
    ctx.strokeStyle = props.stroke;
    ctx.stroke();
  }
  ctx.restore();
}

function drawMotionGraphicPlaceholder(
  ctx: Ctx2D,
  item: Item,
  cx: number,
  cy: number,
  scale: number,
  rotation: number,
): void {
  const name = (item.props as { code: string }).code ? item.labels?.name ?? 'Motion graphic' : 'Motion graphic';
  ctx.save();
  ctx.translate(cx, cy);
  if (rotation !== 0) ctx.rotate((rotation * Math.PI) / 180);
  const w = 480 * scale;
  const h = 270 * scale;
  ctx.fillStyle = 'rgba(59, 130, 246, 0.15)';
  ctx.strokeStyle = '#3b82f6';
  ctx.lineWidth = 2;
  roundedRectPath(ctx, -w / 2, -h / 2, w, h, 12);
  ctx.fill();
  ctx.stroke();
  ctx.fillStyle = '#93c5fd';
  ctx.font = '600 24px system-ui, sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(`▶ ${name}`, 0, 0);
  ctx.restore();
}

function drawMotionError(ctx: Ctx2D, message: string, cx: number, cy: number, canvasW: number, canvasH: number): void {
  ctx.save();
  ctx.translate(cx, cy);
  const w = canvasW * 0.7;
  const h = canvasH * 0.35;
  ctx.fillStyle = 'rgba(127, 29, 29, 0.85)';
  roundedRectPath(ctx, -w / 2, -h / 2, w, h, 12);
  ctx.fill();
  ctx.fillStyle = '#fecaca';
  ctx.font = '600 26px system-ui, sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText('Motion graphic error', 0, -h / 4);
  ctx.font = '16px system-ui, sans-serif';
  wrapText(ctx, message, 0, 0, w - 40, 20);
  ctx.restore();
}

function wrapText(ctx: Ctx2D, text: string, x: number, y: number, maxWidth: number, lineHeight: number): void {
  const words = text.split(/\s+/);
  let line = '';
  let dy = y;
  for (const word of words) {
    const test = line ? `${line} ${word}` : word;
    if (ctx.measureText(test).width > maxWidth && line) {
      ctx.fillText(line, x, dy);
      dy += lineHeight;
      line = word;
    } else {
      line = test;
    }
  }
  if (line) ctx.fillText(line, x, dy);
}

/** Render a motion-graphic scene tree (from the sandbox) onto the canvas. */
async function drawSceneTree(
  ctx: Ctx2D,
  node: MotionNode | null,
  resolver: import('./types.ts').MediaResolver,
  canvasW: number,
  canvasH: number,
): Promise<void> {
  if (!node || typeof node !== 'object') return;
  const rel = (v: number | string | undefined, base: number, fallback = 0): number => {
    if (v === undefined) return fallback;
    if (typeof v === 'string' && v.endsWith('%')) return (parseFloat(v) / 100) * base;
    return typeof v === 'number' ? v : fallback;
  };
  const opacity = Math.min(1, Math.max(0, node.opacity ?? 1));
  if (opacity <= 0) return;
  ctx.save();
  ctx.globalAlpha *= opacity;
  try {
    const x = rel(node.x, canvasW, typeof node.x === 'number' ? node.x : 0);
    const y = rel(node.y, canvasH, typeof node.y === 'number' ? node.y : 0);
    const w = rel(node.width, canvasW, canvasW);
    const h = rel(node.height, canvasH, canvasH);
    if (node.rotation) {
      ctx.translate(x + (typeof node.x === 'number' ? 0 : 0), y);
      ctx.rotate((node.rotation * Math.PI) / 180);
      ctx.translate(-x, -y);
    }
    if (node.type === 'box') {
      ctx.fillStyle = node.fill ?? 'transparent';
      roundedRectPath(ctx, x - w / 2, y - h / 2, w, h, node.radius ?? 0);
      if (node.fill && node.fill !== 'transparent') ctx.fill();
      if (node.strokeWidth && node.strokeColor) {
        ctx.lineWidth = node.strokeWidth;
        ctx.strokeStyle = node.strokeColor;
        ctx.stroke();
      }
    } else if (node.type === 'ellipse') {
      ctx.beginPath();
      ctx.ellipse(x, y, w / 2, h / 2, 0, 0, Math.PI * 2);
      ctx.fillStyle = node.fill ?? '#ffffff';
      ctx.fill();
    } else if (node.type === 'text') {
      let text = node.text ?? '';
      if (node.uppercase) text = text.toUpperCase();
      ctx.font = `${node.fontWeight ?? 800} ${node.fontSize ?? 96}px ${node.fontFamily ?? 'Inter'}, system-ui, sans-serif`;
      ctx.textAlign = (node.align as 'left' | 'center' | 'right') ?? 'center';
      ctx.textBaseline = 'middle';
      if (node.strokeWidth && node.strokeColor) {
        ctx.lineWidth = node.strokeWidth;
        ctx.strokeStyle = node.strokeColor;
        ctx.strokeText(text, x, y);
      }
      ctx.fillStyle = node.color ?? '#ffffff';
      ctx.fillText(text, x, y);
    } else if (node.type === 'img' && node.src) {
      const assetId = node.src.startsWith('asset:') ? node.src.slice(6) : null;
      if (assetId) {
        const media = await safeResolve(resolver, assetId, 0, { type: 'image' } as never);
        if (media) {
          const size = drawableSize(media);
          if (size.width > 0 && size.height > 0) {
            ctx.drawImage(media as CanvasImageSource, x - w / 2, y - h / 2, w, h);
          }
        }
      }
    }
    if (node.children) {
      for (const child of node.children) {
        await drawSceneTree(ctx, child, resolver, canvasW, canvasH);
      }
    }
  } finally {
    ctx.restore();
  }
}

function applyMasks(ctx: Ctx2D, item: Item, canvasW: number, canvasH: number): void {
  // v1: rect/ellipse masks clip to the item's fitted box; path masks use absolute points.
  for (const mask of item.masks) {
    if (mask.shape === 'path' && mask.path && mask.path.length > 1) {
      ctx.beginPath();
      ctx.moveTo(mask.path[0]!.x, mask.path[0]!.y);
      for (const p of mask.path.slice(1)) ctx.lineTo(p.x, p.y);
      ctx.closePath();
      if (mask.invert) {
        ctx.rect(0, 0, canvasW, canvasH);
      }
      ctx.clip(mask.invert ? 'evenodd' : 'nonzero');
    } else {
      const box = itemBox(canvasW, canvasH);
      ctx.beginPath();
      if (mask.shape === 'ellipse') {
        ctx.ellipse(box.x + box.w / 2, box.y + box.h / 2, box.w / 2, box.h / 2, 0, 0, Math.PI * 2);
      } else {
        ctx.rect(box.x, box.y, box.w, box.h);
      }
      ctx.clip();
      if (mask.invert) {
        // approximate inverse via a second full-canvas pass is not possible with a single
        // clip; documented limitation for v1 (feather+invert arrive with the mask tooling).
      }
    }
  }
}

function itemBox(canvasW: number, canvasH: number): { x: number; y: number; w: number; h: number } {
  // default "fit" box used by drawFitted for square-ish media; close enough for v1 masks
  return { x: 0, y: 0, w: canvasW, h: canvasH };
}

function drawSelectionOutline(
  ctx: Ctx2D,
  doc: TimelineDoc,
  item: Item,
  _frame: number,
  canvasW: number,
  canvasH: number,
): void {
  const box = itemBox(canvasW, canvasH);
  ctx.save();
  ctx.strokeStyle = '#8b93ff'; // UI selection accent, not the project's brand color
  ctx.lineWidth = 2;
  ctx.setLineDash([6, 4]);
  ctx.strokeRect(box.x + 1, box.y + 1, box.w - 2, box.h - 2);
  ctx.restore();
}

function drawSafeZones(ctx: Ctx2D, w: number, h: number): void {
  ctx.save();
  ctx.strokeStyle = 'rgba(255,255,255,0.35)';
  ctx.setLineDash([5, 5]);
  ctx.lineWidth = 1;
  ctx.strokeRect(w * 0.05, h * 0.05, w * 0.9, h * 0.9); // action safe
  ctx.strokeRect(w * 0.1, h * 0.1, w * 0.8, h * 0.8); // title safe
  ctx.restore();
}

function drawCheckerboard(ctx: Ctx2D, w: number, h: number): void {
  const size = 16;
  for (let y = 0; y < h; y += size) {
    for (let x = 0; x < w; x += size) {
      ctx.fillStyle = ((x / size + y / size) | 0) % 2 === 0 ? '#2a2a2a' : '#1c1c1c';
      ctx.fillRect(x, y, size, size);
    }
  }
}

function roundedRectPath(
  ctx: Ctx2D,
  x: number,
  y: number,
  w: number,
  h: number,
  r: number,
): void {
  const radius = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + w, y, x + w, y + h, radius);
  ctx.arcTo(x + w, y + h, x, y + h, radius);
  ctx.arcTo(x, y + h, x, y, radius);
  ctx.arcTo(x, y, x + w, y, radius);
  ctx.closePath();
}
