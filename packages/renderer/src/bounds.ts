import type { CaptionStyle, Item, TextStyle, TimelineDoc } from '@cutboard/schema';
import { captionCardAt, captionClockMs } from './captions.ts';
import { activeItemsByDrawOrder } from './compositor.ts';
import { itemOpacityAt, resolveItemProperty } from './keyframes.ts';

/** Width in canvas px of `text` rendered with the CSS font shorthand `font`. */
export type TextMeasure = (text: string, font: string) => number;

export interface BoundsOptions {
  /** real text metrics (canvas measureText); the default is a rough per-character estimate */
  measure?: TextMeasure;
  /** intrinsic media size for video/image items; unknown assets are assumed to fill the canvas */
  mediaSize?: (assetId: string) => { width: number; height: number } | null;
}

/** An item's on-canvas box (canvas pixel space), mirroring how the compositor positions it. */
export interface ItemBounds {
  itemId: string;
  trackId: string;
  type: Item['type'];
  /** center of the box */
  cx: number;
  cy: number;
  /** size after scale, before rotation */
  w: number;
  h: number;
  /** degrees, clockwise, about the item's anchor (== box center for centered items) */
  rotation: number;
  /** the item's transform anchor (what transform.x/y positions) */
  anchorX: number;
  anchorY: number;
  /** anchor position when transform.x/y are 0 */
  baseX: number;
  baseY: number;
  /** combined horizontal/vertical scale factors (scale * scaleX / scale * scaleY) */
  sx: number;
  sy: number;
  /** transform.x/y/scale/rotation are keyframed at this item, so static edits would not show */
  animated: boolean;
  /** the item's track is locked */
  locked: boolean;
}

const defaultMeasure: TextMeasure = (text, font) => {
  const size = /(\d+(?:\.\d+)?)px/.exec(font);
  return text.length * (size ? parseFloat(size[1]!) : 16) * 0.55;
};

const ANIMATABLE = ['transform.x', 'transform.y', 'transform.scale', 'transform.rotation'];

/**
 * Visible items' on-canvas boxes at `frame`, in draw order (bottom first). Items on hidden
 * tracks, fully transparent items, and captions with no card showing are left out; items on
 * locked tracks are included with `locked: true`.
 */
export function itemBoundsAt(
  doc: TimelineDoc,
  frame: number,
  canvasW: number,
  canvasH: number,
  opts: BoundsOptions = {},
): ItemBounds[] {
  const measure = opts.measure ?? defaultMeasure;
  const out: ItemBounds[] = [];
  for (const item of activeItemsByDrawOrder(doc, frame)) {
    if (item.type === 'audio') continue;
    if (itemOpacityAt(item, frame) <= 0) continue;
    const b = boundsForItem(doc, item, frame, canvasW, canvasH, measure, opts.mediaSize);
    if (b) out.push(b);
  }
  return out;
}

function boundsForItem(
  doc: TimelineDoc,
  item: Item,
  frame: number,
  W: number,
  H: number,
  measure: TextMeasure,
  mediaSize: BoundsOptions['mediaSize'],
): ItemBounds | null {
  const t = item.transform;
  const rx = resolveItemProperty(item, 'transform.x', frame);
  const ry = resolveItemProperty(item, 'transform.y', frame);
  const scale = resolveItemProperty(item, 'transform.scale', frame);
  const rotation = resolveItemProperty(item, 'transform.rotation', frame);
  let sx = scale * t.scaleX;
  let sy = scale * t.scaleY;
  let baseX = W / 2;
  let baseY = H / 2;
  let anchorX = W / 2 + rx;
  let anchorY = H / 2 + ry;
  // local (unscaled) rectangle relative to the anchor
  let x0 = 0;
  let x1 = 0;
  let y0 = 0;
  let y1 = 0;

  if (item.type === 'video' || item.type === 'image') {
    const size = item.assetId ? mediaSize?.(item.assetId) : null;
    if (size && size.width > 0 && size.height > 0) {
      const fit = Math.min(W / size.width, H / size.height);
      x1 = size.width * fit;
      y1 = size.height * fit;
    } else {
      x1 = W;
      y1 = H;
    }
    x0 = -x1 / 2;
    x1 = x1 / 2;
    y0 = -y1 / 2;
    y1 = y1 / 2;
  } else if (item.type === 'text') {
    const props = item.props as { text: string; style: TextStyle };
    const style = props.style;
    const font = `${style.fontWeight} ${style.fontSize}px ${style.fontFamily}, Geist, system-ui, sans-serif`;
    const lines = props.text.split('\n').map((l) => (style.uppercase ? l.toUpperCase() : l));
    const widest = Math.max(0, ...lines.map((l) => measure(l, font)));
    const lh = style.fontSize * style.lineHeight;
    const hh = (lines.length * lh) / 2;
    if (style.align === 'left') {
      x0 = 0;
      x1 = widest;
    } else if (style.align === 'right') {
      x0 = -widest;
      x1 = 0;
    } else {
      x0 = -widest / 2;
      x1 = widest / 2;
    }
    y0 = -hh;
    y1 = hh;
    if (style.backgroundColor) {
      // the compositor draws the plate centered on the anchor regardless of alignment
      const pad = style.padding;
      x0 = Math.min(x0, -widest / 2 - pad);
      x1 = Math.max(x1, widest / 2 + pad);
      y0 = Math.min(y0, -hh - pad * 0.5);
      y1 = Math.max(y1, hh + pad * 0.5);
    }
  } else if (item.type === 'caption') {
    const props = item.props as {
      words: { w: string; startMs: number; endMs: number }[];
      style: CaptionStyle;
      maxWordsPerCard: number;
    };
    const style = props.style;
    const shown = captionCardAt(props.words, captionClockMs(frame, item.startFrame, doc.project.fps), props.maxWordsPerCard);
    if (!shown) return null;
    // captions use the uniform scale only, and x/y are static
    sx = scale;
    sy = scale;
    baseY = H * style.placementY;
    anchorX = W / 2 + t.x;
    anchorY = baseY + t.y;
    const font = `${style.fontWeight} ${style.fontSize}px ${style.fontFamily}, Geist, system-ui, sans-serif`;
    const lines: string[][] = [[]];
    let chars = 0;
    for (const word of shown.card) {
      const len = word.w.length + 1;
      if (chars + len > style.maxCharsPerLine && lines[lines.length - 1]!.length > 0) {
        lines.push([]);
        chars = 0;
      }
      lines[lines.length - 1]!.push(word.w);
      chars += len;
    }
    const space = measure(' ', font);
    let widest = 0;
    for (const line of lines) {
      const total = line.reduce((s, w) => s + measure(w, font), 0) + space * (line.length - 1);
      widest = Math.max(widest, total);
    }
    const lh = style.fontSize * style.lineHeight * 1.15;
    // each line is centered on y; glyph height is roughly one font size
    const top = -((lines.length - 1) / 2) * lh - style.fontSize * 0.6;
    const bottom = ((lines.length - 1) / 2) * lh + style.fontSize * 0.6;
    x0 = -widest / 2;
    x1 = widest / 2;
    y0 = top;
    y1 = bottom;
  } else if (item.type === 'shape') {
    const p = item.props as { width: number; height: number };
    x0 = -p.width / 2;
    x1 = p.width / 2;
    y0 = -p.height / 2;
    y1 = p.height / 2;
  } else if (item.type === 'motionGraphic') {
    // the scene tree is laid out against the full canvas and scaled uniformly
    sx = scale;
    sy = scale;
    x0 = -W / 2;
    x1 = W / 2;
    y0 = -H / 2;
    y1 = H / 2;
  } else {
    return null;
  }

  const w = (x1 - x0) * sx;
  const h = (y1 - y0) * sy;
  const lcx = ((x0 + x1) / 2) * sx;
  const lcy = ((y0 + y1) / 2) * sy;
  const r = (rotation * Math.PI) / 180;
  const cos = Math.cos(r);
  const sin = Math.sin(r);
  const track = doc.tracks.find((tr) => tr.id === item.trackId);
  const animated = ANIMATABLE.some((k) => (item.keyframes[k]?.length ?? 0) > 0);
  return {
    itemId: item.id,
    trackId: item.trackId,
    type: item.type,
    cx: anchorX + lcx * cos - lcy * sin,
    cy: anchorY + lcx * sin + lcy * cos,
    w,
    h,
    rotation,
    anchorX,
    anchorY,
    baseX,
    baseY,
    sx,
    sy,
    animated,
    locked: Boolean(track?.locked),
  };
}

/** True when canvas point (px, py) lies inside the (rotated) box. */
export function pointInBounds(b: Pick<ItemBounds, 'cx' | 'cy' | 'w' | 'h' | 'rotation'>, px: number, py: number): boolean {
  const r = (-b.rotation * Math.PI) / 180;
  const dx = px - b.cx;
  const dy = py - b.cy;
  const lx = dx * Math.cos(r) - dy * Math.sin(r);
  const ly = dx * Math.sin(r) + dy * Math.cos(r);
  return Math.abs(lx) <= b.w / 2 && Math.abs(ly) <= b.h / 2;
}

/** The topmost box under the point (`bounds` is in draw order, so search from the end). */
export function hitTestBounds<T extends ItemBounds>(bounds: T[], px: number, py: number): T | null {
  for (let i = bounds.length - 1; i >= 0; i--) {
    if (pointInBounds(bounds[i]!, px, py)) return bounds[i]!;
  }
  return null;
}
