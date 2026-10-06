import type { Asset, Item, TimelineDoc } from '@cutboard/schema';
import { MEDIA_TYPES } from '@cutboard/editor-core';

/**
 * Pure planning helpers for the export pipeline: which media the render window loads and
 * how the audio is mixed. Kept free of Electron imports so they can be unit-tested.
 */

const CHROMIUM_VIDEO_CODECS = new Set(['h264', 'vp8', 'vp9', 'av1']);
const CHROMIUM_VIDEO_EXTS = new Set(['mp4', 'm4v', 'mov', 'webm']);

/**
 * File the hidden render window should load for an asset. The 540p proxy exists so the
 * preview scrubs smoothly; exporting from it would upscale a low-resolution picture, so
 * the original is used whenever Chromium can decode it (known codec + container).
 * Assets imported before the codec was recorded keep using the proxy.
 */
export function exportMediaPath(asset: Pick<Asset, 'kind' | 'path' | 'proxyPath' | 'metadata'>): string {
  if (asset.kind === 'image') return asset.path;
  if (asset.kind === 'video') {
    const ext = asset.path.split('.').pop()?.toLowerCase() ?? '';
    const codec = asset.metadata?.codec?.toLowerCase();
    if (codec && CHROMIUM_VIDEO_CODECS.has(codec) && CHROMIUM_VIDEO_EXTS.has(ext)) return asset.path;
  }
  return asset.proxyPath ?? asset.path;
}

export interface AudioSource {
  item: Item;
  path: string;
}

/** Items that contribute audio: unmuted, on an unmuted track, with a real audio stream. */
export function selectAudioSources(
  doc: TimelineDoc,
  getAsset: (assetId: string) => Pick<Asset, 'path' | 'hasAudio'> | null,
): AudioSource[] {
  const out: AudioSource[] = [];
  for (const item of doc.items) {
    if (!MEDIA_TYPES.includes(item.type) || item.muted || !item.assetId) continue;
    if (doc.tracks.find((t) => t.id === item.trackId)?.muted) continue;
    const asset = getAsset(item.assetId);
    if (!asset || !asset.hasAudio) continue;
    out.push({ item, path: asset.path });
  }
  return out;
}

/** ffmpeg `atempo` only accepts 0.5-2 per instance, so larger changes are chained. */
export function atempoChain(speed: number): string[] {
  const factors: number[] = [];
  let s = speed;
  while (s > 2) {
    factors.push(2);
    s /= 2;
  }
  while (s < 0.5) {
    factors.push(0.5);
    s /= 0.5;
  }
  factors.push(s);
  return factors.map((f) => `atempo=${f.toFixed(5)}`);
}

const num = (n: number) => n.toFixed(4);

function easeExpr(easing: string, u: string): string {
  switch (easing) {
    case 'hold':
      return '0';
    case 'easeIn':
      return `(${u})*(${u})`;
    case 'easeOut':
      return `(1-(1-(${u}))*(1-(${u})))`;
    case 'easeInOut':
      return `if(lt(${u},0.5),2*(${u})*(${u}),1-pow(-2*(${u})+2,2)/2)`;
    default:
      return u;
  }
}

/**
 * Volume as an ffmpeg expression of item-local time `t`, mirroring itemVolumeAt in the
 * preview: keyframes (linear/eased between them) replace the static volume, and fades
 * multiply on top. Returns a plain number string when nothing varies over time.
 */
export function volumeExpression(item: Item, fps: number): string {
  const fadeIn = ((item.props as { fadeInFrames?: number }).fadeInFrames ?? 0) / fps;
  const fadeOut = ((item.props as { fadeOutFrames?: number }).fadeOutFrames ?? 0) / fps;
  const durSec = item.durationFrames / fps;
  const kfs = [...(item.keyframes['volume'] ?? [])].sort((a, b) => a.frame - b.frame);

  let base: string;
  if (kfs.length === 0) {
    base = num(item.volume);
  } else {
    const at = (i: number) => (kfs[i]!.frame / fps).toFixed(4);
    base = num(kfs[kfs.length - 1]!.value);
    for (let i = kfs.length - 2; i >= 0; i--) {
      const a = kfs[i]!;
      const b = kfs[i + 1]!;
      const span = Math.max(1 / fps, (b.frame - a.frame) / fps).toFixed(4);
      const u = `(t-${at(i)})/${span}`;
      const seg = `${num(a.value)}+(${num(b.value - a.value)})*${easeExpr(b.easing, u)}`;
      base = `if(lt(t,${at(i + 1)}),${seg},${base})`;
    }
    base = `if(lt(t,${at(0)}),${num(kfs[0]!.value)},${base})`;
  }

  const factors: string[] = [];
  if (fadeIn > 0) factors.push(`min(1,max(0,t/${fadeIn.toFixed(4)}))`);
  if (fadeOut > 0) factors.push(`min(1,max(0,(${durSec.toFixed(4)}-t)/${fadeOut.toFixed(4)}))`);

  if (kfs.length === 0 && factors.length === 0) return `volume=${num(Math.min(1, Math.max(0, item.volume)))}`;
  return `volume='clip((${[base, ...factors].join(')*(')}),0,1)':eval=frame`;
}

export interface AudioGraph {
  /** `-i` inputs, in order (ffmpeg input 0 is the raw video pipe, so these start at 1). */
  inputs: string[];
  filterComplex: string;
  outLabel: string;
}

/**
 * Mix every audio source onto the timeline. Per source: cut the used part of the file,
 * apply speed, then the volume envelope (item-local time), and only then delay it to its
 * timeline position, so keyframes and fades are not shifted by the item's start.
 */
export function buildAudioGraph(sources: AudioSource[], fps: number): AudioGraph | null {
  if (sources.length === 0) return null;
  const parts = sources.map(({ item }, i) => {
    const sourceIn = item.sourceInFrame ?? 0;
    const startSec = sourceIn / fps;
    const endSec = (sourceIn + item.durationFrames * item.speed) / fps;
    const chain = [
      `atrim=start=${startSec.toFixed(3)}:end=${endSec.toFixed(3)}`,
      'asetpts=PTS-STARTPTS',
      ...(item.speed !== 1 ? atempoChain(item.speed) : []),
      volumeExpression(item, fps),
      `adelay=${Math.round((item.startFrame / fps) * 1000)}:all=1`,
    ];
    return `[${i + 1}:a]${chain.join(',')}[a${i}]`;
  });
  const labels = sources.map((_, i) => `[a${i}]`).join('');
  parts.push(`${labels}amix=inputs=${sources.length}:normalize=0:duration=longest[mixed]`);
  return { inputs: sources.map((s) => s.path), filterComplex: parts.join(';'), outLabel: '[mixed]' };
}
