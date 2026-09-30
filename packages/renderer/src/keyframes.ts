import { Easing, Item, Keyframe } from '@cutboard/schema';

const easeIn = (t: number): number => t * t;
const easeOut = (t: number): number => 1 - (1 - t) * (1 - t);
const easeInOut = (t: number): number => (t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);

function applyEasing(t: number, easing: Easing): number {
  switch (easing) {
    case 'hold':
      return 0;
    case 'easeIn':
      return easeIn(t);
    case 'easeOut':
      return easeOut(t);
    case 'easeInOut':
      return easeInOut(t);
    default:
      return t;
  }
}

/** Interpolate a sorted keyframe list at an item-local frame. */
export function resolveKeyframes(kfs: Keyframe[], localFrame: number): number | null {
  if (kfs.length === 0) return null;
  const sorted = [...kfs].sort((a, b) => a.frame - b.frame);
  const first = sorted[0]!;
  const last = sorted[sorted.length - 1]!;
  if (localFrame <= first.frame) return first.value;
  if (localFrame >= last.frame) {
    return last.value;
  }
  for (let i = 0; i < sorted.length - 1; i++) {
    const a = sorted[i]!;
    const b = sorted[i + 1]!;
    if (localFrame >= a.frame && localFrame <= b.frame) {
      const span = Math.max(1, b.frame - a.frame);
      const t = applyEasing((localFrame - a.frame) / span, b.easing);
      return a.value + (b.value - a.value) * t;
    }
  }
  return last.value;
}

/**
 * Resolve an animatable property. Keyframes win; otherwise fall back to the item's
 * static value. `propertyPath` examples: `transform.x`, `transform.opacity`, `volume`,
 * `transform.scale`, `transform.rotation`.
 */
export function resolveItemProperty(item: Item, propertyPath: string, frame: number): number {
  const localFrame = frame - item.startFrame;
  const kfs = item.keyframes[propertyPath];
  if (kfs && kfs.length > 0) {
    const v = resolveKeyframes(kfs, localFrame);
    if (v !== null) return v;
  }
  const [section, key] = propertyPath.split('.');
  if (section === 'transform') {
    const value = (item.transform as unknown as Record<string, number>)[key!];
    if (typeof value === 'number') return value;
  }
  if (propertyPath === 'volume') return item.volume;
  if (propertyPath === 'speed') return item.speed;
  return 0;
}

/** Item-local opacity including fade in/out props and keyframed opacity. */
export function itemOpacityAt(item: Item, frame: number): number {
  let alpha = resolveItemProperty(item, 'transform.opacity', frame);
  const local = frame - item.startFrame;
  const fadeIn = (item.props as { fadeInFrames?: number }).fadeInFrames ?? 0;
  const fadeOut = (item.props as { fadeOutFrames?: number }).fadeOutFrames ?? 0;
  if (fadeIn > 0 && local < fadeIn) alpha *= Math.max(0, local / fadeIn);
  const fromEnd = item.durationFrames - local;
  if (fadeOut > 0 && fromEnd < fadeOut) alpha *= Math.max(0, fromEnd / fadeOut);
  return Math.min(1, Math.max(0, alpha));
}

/** Volume 0..1 for audio-bearing items including fades and track mute (mute applied by host). */
export function itemVolumeAt(item: Item, frame: number): number {
  let vol = resolveItemProperty(item, 'volume', frame);
  const local = frame - item.startFrame;
  const fadeIn = (item.props as { fadeInFrames?: number }).fadeInFrames ?? 0;
  const fadeOut = (item.props as { fadeOutFrames?: number }).fadeOutFrames ?? 0;
  if (fadeIn > 0 && local < fadeIn) vol *= Math.max(0, local / fadeIn);
  const fromEnd = item.durationFrames - local;
  if (fadeOut > 0 && fromEnd < fadeOut) vol *= Math.max(0, fromEnd / fadeOut);
  return Math.min(1, Math.max(0, vol));
}
