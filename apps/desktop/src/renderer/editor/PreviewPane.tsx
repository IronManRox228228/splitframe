import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { CanvasCompositor, itemVolumeAt } from '@cutboard/renderer';
import { itemEnd, sourceFrameAt, docDurationFrames } from '@cutboard/editor-core';
import { formatTimecode } from '@cutboard/schema';
import type { MediaPool } from '../lib/media.ts';
import { Icon } from '../ui/Icon.tsx';
import { CommandBar } from './CommandBar.tsx';
import { latestOnly, needsResync, planAudio, planVideo } from '../lib/playback-plan.ts';
import { CanvasOverlay, type LiveMap, type OverlayRect } from './canvas/CanvasOverlay.tsx';
import type { TimelineDoc } from '@cutboard/schema';

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
  const stepFrames = useEditor((s) => s.stepFrames);
  const importMedia = useEditor((s) => s.importMedia);
  const muted = useEditor((s) => s.previewMuted);
  const setMuted = useEditor((s) => s.setPreviewMuted);
  const [showSafeZones, setShowSafeZones] = useState(false);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const compositorRef = useRef(new CanvasCompositor());
  const rafRef = useRef<number>(0);
  const lastTickRef = useRef<number>(0);
  const stageRef = useRef<HTMLDivElement>(null);
  // transforms being dragged on the canvas: drawn live, committed once on pointer-up
  const [live, setLive] = useState<LiveMap | null>(null);
  const liveRef = useRef<LiveMap | null>(null);
  liveRef.current = live;
  const [rect, setRect] = useState<OverlayRect | null>(null);
  const [sizeTick, setSizeTick] = useState(0);

  const drawNow = async ({ frame, live }: { frame: number; live: boolean }) => {
    const canvas = canvasRef.current;
    const state = useEditor.getState();
    const pool = state.mediaPool;
    if (!canvas || !state.doc || !pool) return;
    // project resolution, like export, so x/y/font sizes mean the same thing in both
    canvas.width = Math.max(1, state.doc.project.width);
    canvas.height = Math.max(1, state.doc.project.height);
    await compositorRef.current.draw(canvas, withLive(state.doc, liveRef.current, true), frame, pool, {
      showSafeZones,
    });
    if (!live) setSizeTick((n) => n + 1);
  };
  // draws reset and repaint one shared canvas, so they run one at a time and a burst of
  // requests (scrubbing, playback ticks) collapses to the newest frame
  const drawNowRef = useRef(drawNow);
  drawNowRef.current = drawNow;
  const runDraw = useMemo(() => latestOnly((arg: { frame: number; live: boolean }) => drawNowRef.current(arg)), []);
  const renderFrame = useCallback((frame: number, live: boolean) => runDraw({ frame, live }), [runDraw]);

  // paused / scrub rendering (exact frames via seeks)
  useEffect(() => {
    if (playing) return;
    void renderFrame(playhead, false);
  }, [doc, playhead, live, playing, showSafeZones, renderFrame]);

  // keep the overlay glued to the canvas as the layout or aspect changes
  const hasDoc = doc !== null;
  useEffect(() => {
    const stage = stageRef.current;
    const canvas = canvasRef.current;
    if (!stage || !canvas) return;
    const measure = () => {
      const s = stage.getBoundingClientRect();
      const c = canvas.getBoundingClientRect();
      const next = { left: c.left - s.left, top: c.top - s.top, width: c.width, height: c.height };
      setRect((prev) =>
        prev && prev.left === next.left && prev.top === next.top && prev.width === next.width && prev.height === next.height
          ? prev
          : next,
      );
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(stage);
    ro.observe(canvas);
    return () => ro.disconnect();
  }, [hasDoc]);

  // preview mute: mirrors the toggle onto every media element the pool owns
  useEffect(() => {
    applyMute(useEditor.getState().mediaPool, muted);
  }, [muted, playing]);

  // playback loop
  useEffect(() => {
    if (!playing) return;
    const startDoc = useEditor.getState().doc;
    const pool = useEditor.getState().mediaPool;
    if (!startDoc || !pool) return;
    const fps = startDoc.project.fps;
    // pressing play at the very end restarts from the beginning
    if (useEditor.getState().playhead >= docDurationFrames(startDoc)) useEditor.getState().setPlayhead(0);
    let playheadFrames = useEditor.getState().playhead;
    let cancelled = false;

    const start = async () => {
      const state = useEditor.getState();
      pool.liveMode = true;
      // position every active video at the playhead, then let them play
      applyMute(pool, useEditor.getState().previewMuted);
      await Promise.all(syncVideo(startDoc, pool, state.playhead, fps));
      syncAudio(startDoc, pool, state.playhead, fps);
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
        // clips entering view mid-playback start playing instead of being re-seeked every frame
        applyMute(pool, s.previewMuted);
        syncVideo(s.doc, pool, playheadFrames, fps);
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
  const fps = doc.project.fps;
  const empty = doc.items.length === 0;

  return (
    <main className="flex-1 min-w-0 min-h-0 relative flex flex-col items-center bg-surface-950">
      <CommandBar />
      <div ref={stageRef} className="flex-1 min-h-0 w-full flex items-center justify-center px-6 pt-[76px] pb-4 relative">
        <canvas
          ref={canvasRef}
          className="max-h-full max-w-full rounded-md bg-black"
          style={{ boxShadow: '0 0 0 1px #1c1c20' }}
        />
        {!empty && rect && (
          <CanvasOverlay
            doc={withLive(doc, live, false)}
            frame={Math.round(playhead)}
            canvasW={doc.project.width}
            canvasH={doc.project.height}
            rect={rect}
            sizeTick={sizeTick}
            mediaSize={(id) => useEditor.getState().mediaPool?.sizeOf(id) ?? null}
            onLive={setLive}
          />
        )}
        {empty && (
          <div className="absolute inset-0 pt-[76px] flex items-center justify-center pointer-events-none">
            <div className="flex flex-col items-center gap-3 text-center pointer-events-auto">
              <p className="text-fg text-sm font-medium">Add media to start</p>
              <p className="text-xs text-fg-muted">Your preview appears here.</p>
              <button className="btn-primary" onClick={() => void importMedia()}>
                <Icon name="upload" size={16} />
                Import media
              </button>
            </div>
          </div>
        )}
      </div>
      <div className="mb-4 flex items-center gap-1.5 p-1.5 rounded-[14px] bg-surface-850 border border-surface-800">
        <button className="icon-btn-sm" aria-label="Previous frame" title="Previous frame (Left)" onClick={() => stepFrames(-1)}>
          <Icon name="skipStart" size={16} />
        </button>
        <button
          className="w-9 h-9 rounded-[10px] bg-primary text-surface-950 flex items-center justify-center hover:bg-primary-hover"
          aria-label={playing ? 'Pause' : 'Play'}
          title={playing ? 'Pause (Space)' : 'Play (Space)'}
          onClick={() => setPlaying(!playing)}
        >
          <Icon name={playing ? 'pause' : 'play'} size={16} />
        </button>
        <button className="icon-btn-sm" aria-label="Next frame" title="Next frame (Right)" onClick={() => stepFrames(1)}>
          <Icon name="skipEnd" size={16} />
        </button>
        <span className="px-2.5 font-mono text-xs text-fg tabular-nums whitespace-nowrap">
          {formatTimecode(Math.round(playhead), fps)} <span className="text-fg-faint">/ {formatTimecode(total, fps)}</span>
        </span>
        <button
          className="icon-btn-sm"
          aria-label={muted ? 'Unmute preview' : 'Mute preview'}
          aria-pressed={muted}
          title={muted ? 'Unmute preview' : 'Mute preview'}
          onClick={() => setMuted(!muted)}
        >
          <Icon name={muted ? 'mute' : 'volume'} size={16} />
        </button>
        <button
          className={`icon-btn-sm ${showSafeZones ? 'text-accent' : ''}`}
          aria-label="Safe zones"
          aria-pressed={showSafeZones}
          title={showSafeZones ? 'Hide safe zones' : 'Show safe zones'}
          onClick={() => setShowSafeZones(!showSafeZones)}
        >
          <Icon name="layers" size={16} />
        </button>
        <button
          className="icon-btn-sm"
          aria-label="Full screen"
          title="Full screen preview"
          onClick={() => {
            if (document.fullscreenElement) void document.exitFullscreen();
            else void canvasRef.current?.requestFullscreen?.();
          }}
        >
          <Icon name="fullscreen" size={16} />
        </button>
      </div>
    </main>
  );
}

function applyMute(pool: MediaPool | null, muted: boolean): void {
  if (!pool) return;
  for (const el of pool.allAudio) if (el.muted !== muted) el.muted = muted;
  for (const el of pool.allVideoElements) if (el.muted !== muted) el.muted = muted;
}

/** The doc with in-flight drag transforms merged in (opacity only when drawing, so a hidden item keeps its box). */
function withLive(doc: TimelineDoc, live: LiveMap | null, includeOpacity: boolean): TimelineDoc {
  if (!live) return doc;
  return {
    ...doc,
    items: doc.items.map((item) => {
      const t = live[item.id];
      if (!t) return item;
      const { opacity, ...rest } = t;
      const merged = { ...item.transform, ...rest, ...(includeOpacity && opacity !== undefined ? { opacity } : {}) };
      return { ...item, transform: merged };
    }),
  };
}

type EditorDoc = NonNullable<ReturnType<typeof useEditor.getState>['doc']>;

/** Start/stop video elements so each follows the single item currently using its asset. */
function syncVideo(doc: EditorDoc, pool: MediaPool, frame: number, fps: number): Promise<unknown>[] {
  const plays: Promise<unknown>[] = [];
  for (const [assetId, item] of planVideo(doc, frame)) {
    const el = pool.videoElements.get(assetId);
    if (!el) continue; // not drawn yet; the paused-path draw creates it and the next tick starts it
    if (!item) {
      if (!el.paused) el.pause();
      continue;
    }
    const expected = sourceFrameAt(item, Math.floor(frame)) / fps;
    el.playbackRate = Math.min(4, Math.max(0.25, item.speed));
    if (el.paused) {
      el.currentTime = expected;
      plays.push(el.play().catch(() => undefined));
    } else if (needsResync(el.currentTime, expected)) {
      el.currentTime = expected; // e.g. a cut that jumps within the same source file
    }
  }
  return plays;
}

/**
 * Audio elements are shared per asset (a split clip's halves use the same one), so the
 * plan picks one item per asset and the element is paused only when none is active.
 */
function syncAudio(doc: EditorDoc, pool: MediaPool, frame: number, fps: number): void {
  for (const [assetId, item] of planAudio(doc, frame)) {
    const el = pool.getAudioElement(assetId);
    if (!el) continue;
    if (!item) {
      if (!el.paused) el.pause();
      continue;
    }
    const expected = sourceFrameAt(item, Math.floor(frame)) / fps;
    el.playbackRate = Math.min(4, Math.max(0.25, item.speed));
    el.volume = Math.min(1, Math.max(0, itemVolumeAt(item, frame)));
    if (el.paused) {
      el.currentTime = expected;
      void el.play().catch(() => undefined);
    } else if (needsResync(el.currentTime, expected)) {
      el.currentTime = expected;
    }
  }
}
