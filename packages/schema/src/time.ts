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
  const totalSeconds = Math.max(0, frames) / fps;
  const m = Math.floor(totalSeconds / 60);
  const s = totalSeconds - m * 60;
  return `${String(m).padStart(2, '0')}:${s.toFixed(1).padStart(4, '0')}`;
}

export const easingSchema = z.enum(['linear', 'hold', 'easeIn', 'easeOut', 'easeInOut']);
export type Easing = z.infer<typeof easingSchema>;

export const keyframeSchema = z.object({
  frame: frameSchema,
  value: z.number(),
  easing: easingSchema.default('linear'),
});
export type Keyframe = z.infer<typeof keyframeSchema>;
