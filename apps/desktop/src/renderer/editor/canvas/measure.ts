import type { TextMeasure } from '@cutboard/renderer';

let ctx: CanvasRenderingContext2D | null = null;

/** Text metrics from a scratch 2D context, so hit boxes match what the compositor draws. */
export const measureText: TextMeasure = (text, font) => {
  if (!ctx) ctx = document.createElement('canvas').getContext('2d');
  if (!ctx) return text.length * 10;
  ctx.font = font;
  return ctx.measureText(text).width;
};
