import { z } from 'zod';

/**
 * All timeline time is integer frames (main prompt §4). Analysis timestamps (transcripts,
 * scenes, beat maps) are integer milliseconds. Convert with the project fps.
 */
export const frameSchema = z.number().int();
export const msSchema = z.number().int();

export function secondsToFrames(seconds: number, fps: number): number {
  return Math.round(seconds * fps);
}

export function framesToSeconds(frames: number, fps: number): number {
  return frames / fps;
}

export function framesToMs(frames: number, fps: number): number {
  return Math.round((frames / fps) * 1000);
}

export function msToFrames(ms: number, fps: number): number {
  return Math.round((ms / 1000) * fps);
}

/** "00:07.2" / "01:02:03.0" style timecode */
export function formatTimecode(frames: number, fps: number): string {
  // round to tenths first so 59.96s carries to 01:00.0 instead of printing 00:60.0
  const tenths = Math.round((Math.max(0, frames) / fps) * 10);
  const t = tenths % 10;
  const totalSeconds = Math.floor(tenths / 10);
  const s = totalSeconds % 60;
  const m = Math.floor(totalSeconds / 60) % 60;
  const h = Math.floor(totalSeconds / 3600);
  const pad = (n: number) => String(n).padStart(2, '0');
  return h > 0 ? `${pad(h)}:${pad(m)}:${pad(s)}.${t}` : `${pad(m)}:${pad(s)}.${t}`;
}

export const easingSchema = z.enum(['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut']);
export type Easing = z.infer<typeof easingSchema>;

export const keyframeSchema = z.object({
  frame: frameSchema,
  value: z.number(),
  easing: easingSchema.default('linear'),
});
export type Keyframe = z.infer<typeof keyframeSchema>;
