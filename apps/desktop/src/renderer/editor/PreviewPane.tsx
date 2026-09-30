import { useCallback, useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { CanvasCompositor, activeItemsByDrawOrder, itemVolumeAt } from '@cutboard/renderer';
import { isAudioBearing, itemEnd, sourceFrameAt, docDurationFrames } from '@cutboard/editor-core';
import { formatTimecode } from '@cutboard/schema';
import type { MediaPool } from '../lib/media.ts';

/**
 * Preview: paused scrubbing renders via seeked media (exact frames); playback advances
 * the playhead in real time, plays video elements natively (liveMode skips seeks), and
 * keeps the audio pool's per-item volumes and fades in sync.
 */
export function PreviewPane() {
  const doc = useEditor((s) => s.doc);
  const playhead = useEditor((s) => s.playhead);
  const playing = useEditor((s) => s.playing);
  const setPlaying = useEditor((s) => s.setPlaying);
  const setPlayhead = useEditor((s) => s.setPlayhead);
  const selection = useEditor((s) => s.selection);
  const [showSafeZones, setShowSafeZones] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const compositorRef = useRef(new CanvasCompositor());
  const rafRef = useRef<number>(0);
  const lastTickRef = useRef<number>(0);

  const renderFrame = useCallback(
    async (frame: number, live: boolean) => {
      const canvas = canvasRef.current;
      const state = useEditor.getState();
      const pool = state.mediaPool;
      if (!canvas || !state.doc || !pool) return;
      canvas.width = 960;
      canvas.height = Math.max(1, Math.round((960 * state.doc.project.height) / state.doc.project.width));
      await compositorRef.current.draw(canvas, state.doc, frame, pool, {
        selectedIds: live ? [] : state.selection,
        showSafeZones,
      });
    },
    [showSafeZones],
  );

  // paused / scrub rendering (exact frames via seeks)
  useEffect(() => {
    if (playing) return;
    void renderFrame(playhead, false);
  }, [doc, playhead, selection, playing, renderFrame]);

  // playback loop
  useEffect(() => {
    if (!playing) return;
    const startDoc = useEditor.getState().doc;
    const pool = useEditor.getState().mediaPool;
    if (!startDoc || !pool) return;
    const fps = startDoc.project.fps;
    let playheadFrames = useEditor.getState().playhead;
    let cancelled = false;

    const start = async () => {
      const state = useEditor.getState();
      const active = activeItemsByDrawOrder(startDoc, state.playhead);
      pool.liveMode = true;
      // position every active video at the playhead, then let them play
      for (const item of active) {
        if (item.type !== 'video' || !item.assetId) continue;
        const el = pool.videoElements.get(item.assetId);
        if (!el) continue;
        el.playbackRate = Math.min(4, Math.max(0.25, item.speed));
        el.currentTime = sourceFrameAt(item, state.playhead) / fps;
      }
      startAudio(startDoc, pool, state.playhead, fps);
      await Promise.all(active.filter((i) => i.type === 'video').map((i) => pool.videoElements.get(i.assetId ?? '')?.play().catch(() => undefined)));
      lastTickRef.current = performance.now();
      const tick = (now: number) => {
        if (cancelled) return;
        const s = useEditor.getState();
        if (!s.playing || !s.doc) return;
        const dt = Math.min(0.25, (now - lastTickRef.current) / 1000);
        lastTickRef.current = now;
        const total = docDurationFrames(s.doc);
        playheadFrames += dt * s.doc.project.fps;
        if (playheadFrames >= total) {
          s.setPlaying(false);
          s.setPlayhead(total);
          return;
        }
        s.setPlayhead(playheadFrames);
        syncAudio(s.doc, pool, playheadFrames, fps);
        void renderFrame(playheadFrames, true);
        rafRef.current = requestAnimationFrame(tick);
      };
      rafRef.current = requestAnimationFrame(tick);
    };
    void start();

    return () => {
      cancelled = true;
      cancelAnimationFrame(rafRef.current);
      const state = useEditor.getState();
      const poolNow = state.mediaPool;
      if (poolNow) {
        poolNow.liveMode = false;
        for (const el of poolNow.allAudio) el.pause();
        for (const el of poolNow.allVideoElements) el.pause();
        const d = state.doc;
        const f = Math.round(state.playhead);
        if (d) {
          for (const item of d.items) {
            if (item.type !== 'video' || !item.assetId) continue;
            const el = poolNow.videoElements.get(item.assetId);
            if (el && f >= item.startFrame && f < itemEnd(item)) {
              el.currentTime = sourceFrameAt(item, f) / d.project.fps;
            }
          }
        }
      }
      lastTickRef.current = 0;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [playing]);

  if (!doc) return null;
  const total = docDurationFrames(doc);

  return (
    <div className="flex-1 flex flex-col min-h-0 bg-surface-950">
      <div className="flex-1 flex items-center justify-center p-4 min-h-0">
        <canvas
          ref={canvasRef}
          className="max-h-full max-w-full rounded-lg border border-line shadow-2xl shadow-black/60"
        />
      </div>
      <div className="h-11 shrink-0 border-t border-line bg-surface-900 flex items-center gap-3 px-4">
        <button
          className="btn-ghost w-8 px-0 justify-center text-sm"
          title={playing ? 'Pause (Space)' : 'Play (Space)'}
          onClick={() => setPlaying(!playing)}
        >
          {playing ? '⏸' : '▶'}
        </button>
        <span className="text-xs text-neutral-300 font-mono tabular-nums w-24">
          {formatTimecode(Math.round(playhead), doc.project.fps)}
        </span>
        <span className="text-[11px] text-neutral-600 font-mono">/ {formatTimecode(total, doc.project.fps)}</span>
        <div className="flex-1" />
        <label className="flex items-center gap-1.5 text-[11px] text-neutral-500 cursor-pointer">
          <input
            type="checkbox"
            checked={showSafeZones}
            onChange={(e) => setShowSafeZones(e.target.checked)}
            className="accent-accent"
          />
          Safe zones
        </label>
        <button
          className="btn-ghost"
          title="Jump to end"
          onClick={() => {
            setPlaying(false);
            setPlayhead(total);
          }}
        >
          ⤓
        </button>
      </div>
    </div>
  );
}

function startAudio(doc: NonNullable<ReturnType<typeof useEditor.getState>['doc']>, pool: MediaPool, frame: number, fps: number): void {
  for (const item of doc.items) {
    if (!isAudioBearing(item) || item.muted) continue;
    const track = doc.tracks.find((t) => t.id === item.trackId);
    if (track?.muted) continue;
    const el = pool.getAudioElement(item.assetId ?? '');
    if (!el) continue;
    const startWithin = Math.max(item.startFrame, Math.floor(frame));
    el.currentTime = Math.max(0, sourceFrameAt(item, startWithin) / fps);
    el.volume = Math.min(1, Math.max(0, itemVolumeAt(item, frame)));
    if (frame >= item.startFrame && frame < itemEnd(item)) {
      void el.play().catch(() => undefined);
    }
  }
}

function syncAudio(doc: NonNullable<ReturnType<typeof useEditor.getState>['doc']>, pool: MediaPool, frame: number, fps: number): void {
  for (const item of doc.items) {
    if (!isAudioBearing(item) || item.muted) continue;
    const track = doc.tracks.find((t) => t.id === item.trackId);
    const el = pool.getAudioElement(item.assetId ?? '');
    if (!el) continue;
    const active = frame >= item.startFrame && frame < itemEnd(item);
    if (active && !track?.muted) {
      el.playbackRate = Math.min(4, Math.max(0.25, item.speed));
      el.volume = Math.min(1, Math.max(0, itemVolumeAt(item, frame)));
      if (el.paused) {
        el.currentTime = sourceFrameAt(item, Math.floor(frame)) / fps;
        void el.play().catch(() => undefined);
      }
    } else if (!el.paused) {
      el.pause();
    }
  }
}
