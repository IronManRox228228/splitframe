import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Item, Track, TimelineDoc, formatTimecode } from '@cutboard/schema';
import { docDurationFrames, itemEnd, trackAllowsItem, getSnapCandidates, snapFrame } from '@cutboard/editor-core';
import { mediaUrl } from '../lib/url.ts';
import {
  HEADER_W,
  ROW_PAD,
  dragCommit,
  formatDurationBadge,
  frameFromPointer,
  marqueeHits,
  rowIndexAtY,
  rulerInterval,
  rulerSubdivisions,
  scrollForZoom,
  trackLayout,
  trimHandleWidth,
  type DragState,
} from '../lib/timeline-math.ts';
import { deleteKeyLabel, shortcutLabel } from '../lib/platform-labels.ts';
import { Icon, type IconName } from '../ui/Icon.tsx';

const RULER_H = 32;
const TOOLBAR_H = 44;
const MIN_PX = 0.2;
const MAX_PX = 40;
const LOG_MIN = Math.log(MIN_PX);
const LOG_MAX = Math.log(MAX_PX);

type Doc = TimelineDoc;

interface Marquee {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

/** Scroll the viewport while the pointer sits near its left/right edge; returns a stop function. */
function startEdgeScroll(el: HTMLElement, getX: () => number, onScrolled: () => void): () => void {
  let raf = 0;
  const tick = () => {
    const r = el.getBoundingClientRect();
    const x = getX();
    const zone = 40;
    const leftEdge = r.left + HEADER_W + zone;
    let dx = 0;
    if (x > r.right - zone) dx = Math.min(24, (x - (r.right - zone)) / 2);
    else if (x < leftEdge) dx = -Math.min(24, (leftEdge - x) / 2);
    if (dx !== 0) {
      const before = el.scrollLeft;
      el.scrollLeft += dx;
      if (el.scrollLeft !== before) onScrolled();
    }
    raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);
  return () => cancelAnimationFrame(raf);
}

function itemLabel(item: Item, assetName: string | undefined): string {
  if (item.type === 'text') {
    const text = (item.props as { text?: string }).text?.trim().split('\n')[0];
    if (text) return text;
  }
  return assetName ?? item.labels?.name ?? item.type;
}

export function Timeline() {
  const doc = useEditor((s) => s.doc);
  const playhead = useEditor((s) => s.playhead);
  const playing = useEditor((s) => s.playing);
  const pxPerFrame = useEditor((s) => s.pxPerFrame);
  const fitNonce = useEditor((s) => s.fitNonce);
  const setPlayhead = useEditor((s) => s.setPlayhead);
  const setPlaying = useEditor((s) => s.setPlaying);
  const selection = useEditor((s) => s.selection);
  const select = useEditor((s) => s.select);
  const setSelection = useEditor((s) => s.setSelection);
  const applyOps = useEditor((s) => s.applyOps);
  const snapEnabled = useEditor((s) => s.snapEnabled);
  const rippleEnabled = useEditor((s) => s.rippleEnabled);
  const toggleSnap = useEditor((s) => s.toggleSnap);
  const toggleRipple = useEditor((s) => s.toggleRipple);
  const setPxPerFrame = useEditor((s) => s.setPxPerFrame);
  const zoomFit = useEditor((s) => s.zoomFit);
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);
  const showToast = useEditor((s) => s.showToast);
  const platform = useEditor((s) => s.appInfo?.platform);

  const scrollRef = useRef<HTMLDivElement>(null);
  const lanesRef = useRef<HTMLDivElement>(null);
  const rootRef = useRef<HTMLElement>(null);
  const [drag, setDrag] = useState<DragState | null>(null);
  const [marquee, setMarquee] = useState<Marquee | null>(null);
  const [userHeight, setUserHeight] = useState<number | null>(null);
  // the window-level handlers are registered once per gesture; they read live state from refs
  const dragRef = useRef<DragState | null>(null);
  const collapseRef = useRef<string | null>(null);
  const docRef = useRef<Doc | null>(doc);
  docRef.current = doc;
  const pendingAnchor = useRef<{ frame: number; viewX: number } | null>(null);
  const prevPx = useRef(pxPerFrame);
  const fitSeen = useRef(fitNonce);

  const frameFromClientX = useCallback((clientX: number): number => {
    const el = lanesRef.current;
    if (!el) return 0;
    return frameFromPointer(clientX, el.getBoundingClientRect().left, useEditor.getState().pxPerFrame);
  }, []);

  // ---- zoom anchoring: keep the playhead (or pointer) put when the zoom changes ----
  useLayoutEffect(() => {
    const el = scrollRef.current;
    if (fitSeen.current !== fitNonce) {
      fitSeen.current = fitNonce;
      prevPx.current = pxPerFrame;
      pendingAnchor.current = null;
      if (el) el.scrollLeft = 0;
      return;
    }
    const prev = prevPx.current;
    prevPx.current = pxPerFrame;
    if (!el || prev === pxPerFrame) return;
    let anchor = pendingAnchor.current;
    pendingAnchor.current = null;
    if (!anchor) {
      const ph = useEditor.getState().playhead;
      const viewX = HEADER_W + ph * prev - el.scrollLeft;
      if (viewX >= HEADER_W && viewX <= el.clientWidth) anchor = { frame: ph, viewX };
      else {
        const cx = el.clientWidth / 2;
        anchor = { frame: (el.scrollLeft + cx - HEADER_W) / prev, viewX: cx };
      }
    }
    el.scrollLeft = scrollForZoom(anchor.frame, anchor.viewX, pxPerFrame);
  }, [pxPerFrame, fitNonce]);

  // ---- keep the playhead visible (playback, keyboard navigation) ----
  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const x = HEADER_W + playhead * pxPerFrame;
    if (x < el.scrollLeft + HEADER_W || x > el.scrollLeft + el.clientWidth - 8) {
      el.scrollLeft = Math.max(0, x - HEADER_W - 32);
    }
  }, [playhead, pxPerFrame, playing]);

  // ---- wheel: Ctrl/Cmd zooms around the pointer, Shift scrolls sideways ----
  const hasDoc = doc !== null;
  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      if (e.ctrlKey || e.metaKey) {
        e.preventDefault();
        const px = useEditor.getState().pxPerFrame;
        const next = Math.min(MAX_PX, Math.max(MIN_PX, px * Math.exp(-e.deltaY * 0.0015)));
        const rect = el.getBoundingClientRect();
        const viewX = e.clientX - rect.left;
        pendingAnchor.current = { frame: (el.scrollLeft + viewX - HEADER_W) / px, viewX };
        useEditor.getState().setPxPerFrame(next);
      } else if (e.shiftKey) {
        e.preventDefault();
        el.scrollLeft += e.deltaY || e.deltaX;
      }
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, [hasDoc]);

  // ---- clip drags and trims (window-level so fast drags don't lose the pointer) ----
  useEffect(() => {
    if (!drag) return;
    const scrollEl = scrollRef.current;
    const last = { x: 0, y: 0, seen: false };

    const applyMove = (clientX: number, clientY: number) => {
      const d = dragRef.current;
      const current = docRef.current;
      const lanes = lanesRef.current;
      if (!d || !current || !lanes) return;
      const item = current.items.find((i) => i.id === d.itemId);
      if (!item) return;
      const state = useEditor.getState();
      const frame = frameFromClientX(clientX);
      const threshold = Math.max(1, Math.round(8 / state.pxPerFrame));
      const exclude = d.groupIds ?? [item.id];
      let next: Partial<DragState> = {};
      if (d.kind === 'move') {
        let rawStart = Math.max(0, frame - d.grabOffsetFrames);
        if (d.groupOrigStarts) {
          const minOrig = Math.min(...Object.values(d.groupOrigStarts), d.origStart);
          rawStart = Math.max(rawStart, d.origStart - minOrig);
        }
        let start = rawStart;
        let guide: number | null = null;
        if (state.snapEnabled) {
          const cands = getSnapCandidates(current, { excludeItemIds: exclude, includePlayhead: state.playhead });
          const inSnap = snapFrame(rawStart, cands, threshold);
          const outSnap = snapFrame(rawStart + item.durationFrames, cands, threshold);
          if (inSnap && (!outSnap || Math.abs(inSnap.delta) <= Math.abs(outSnap.delta))) {
            start = inSnap.frame;
            guide = inSnap.frame;
          } else if (outSnap) {
            start = Math.max(0, outSnap.frame - item.durationFrames);
            guide = start === outSnap.frame - item.durationFrames ? outSnap.frame : null;
          }
          if (d.groupOrigStarts) {
            const minOrig = Math.min(...Object.values(d.groupOrigStarts), d.origStart);
            if (start < d.origStart - minOrig) {
              start = d.origStart - minOrig;
              guide = null;
            }
          }
        }
        const rect = lanes.getBoundingClientRect();
        const layout = trackLayout(current.tracks.map((t) => t.kind));
        const idx = rowIndexAtY(layout, clientY - rect.top);
        const hovered = idx >= 0 ? current.tracks[idx] : undefined;
        const single = (d.groupIds?.length ?? 1) <= 1;
        const origTrack = current.tracks.find((t) => t.id === d.origTrackId) ?? current.tracks[0]!;
        const ok = hovered && single && trackAllowsItem(hovered.kind, item.type) && !hovered.locked;
        next = {
          ghostStart: start,
          ghostTrackId: ok ? hovered.id : origTrack.id,
          guideFrame: guide,
          hoverTrackId: hovered?.id,
        };
      } else {
        let target = frame;
        let guide: number | null = null;
        if (state.snapEnabled) {
          const cands = getSnapCandidates(current, { excludeItemIds: [item.id], includePlayhead: state.playhead });
          const hit = snapFrame(frame, cands, threshold);
          if (hit) {
            target = hit.frame;
            guide = hit.frame;
          }
        }
        next = { ghostFrame: target, guideFrame: guide };
      }
      dragRef.current = { ...d, ...next } as DragState;
      setDrag(dragRef.current);
    };

    const onMove = (e: PointerEvent) => {
      last.x = e.clientX;
      last.y = e.clientY;
      last.seen = true;
      applyMove(e.clientX, e.clientY);
    };
    const finish = (commit: boolean) => {
      const final = dragRef.current;
      const result = commit && final ? dragCommit(final, useEditor.getState().rippleEnabled) : null;
      if (result) void useEditor.getState().applyOps(result.ops, result.label);
      else if (commit && collapseRef.current) useEditor.getState().select(collapseRef.current);
      collapseRef.current = null;
      dragRef.current = null;
      setDrag(null);
    };
    const onUp = () => finish(true);
    const onCancel = () => finish(false);
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp, { once: true });
    window.addEventListener('pointercancel', onCancel, { once: true });
    const stopScroll = scrollEl ? startEdgeScroll(scrollEl, () => last.x, () => last.seen && applyMove(last.x, last.y)) : () => {};
    return () => {
      stopScroll();
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onCancel);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [drag?.itemId, drag?.kind, drag?.pointerId]);

  if (!doc) return null;
  const total = docDurationFrames(doc);
  const fps = doc.project.fps;
  const contentFrames = Math.max(total + Math.round(160 / pxPerFrame), Math.round(240 / pxPerFrame));
  const layout = trackLayout(doc.tracks.map((t) => t.kind));
  const lanesHeight = layout.length > 0 ? layout[layout.length - 1]!.top + layout[layout.length - 1]!.height : 56;
  const fitHeight = TOOLBAR_H + RULER_H + lanesHeight + 14;
  const maxHeight = Math.max(180, Math.round((typeof window !== 'undefined' ? window.innerHeight : 900) * 0.6));
  const height = userHeight !== null ? Math.min(Math.max(120, userHeight), maxHeight + 120) : Math.min(Math.max(fitHeight, 150), maxHeight);

  const startDrag = (e: React.PointerEvent, item: Item, kind: DragState['kind']) => {
    if (e.button !== 0) return;
    e.stopPropagation();
    const track = doc.tracks.find((t) => t.id === item.trackId);
    if (track?.locked) {
      select(item.id, e.shiftKey);
      showToast(`Track "${track.name}" is locked. Unlock it to edit its clips.`, { kind: 'error' });
      return;
    }
    const wasSelected = selection.includes(item.id);
    let nextSelection = selection;
    if (e.shiftKey) {
      select(item.id, true);
      if (wasSelected) return; // shift-click on a selected clip only deselects it
      nextSelection = [...selection, item.id];
    } else if (!wasSelected) {
      select(item.id);
      nextSelection = [item.id];
    }
    setPlaying(false);

    let groupIds: string[] | undefined;
    let groupOrigStarts: Record<string, number> | undefined;
    collapseRef.current = null;
    if (kind === 'move' && nextSelection.length > 1) {
      const lockedIds = new Set(doc.tracks.filter((t) => t.locked).map((t) => t.id));
      const members = doc.items.filter((i) => nextSelection.includes(i.id) && !lockedIds.has(i.trackId));
      groupIds = members.map((i) => i.id);
      groupOrigStarts = Object.fromEntries(members.map((i) => [i.id, i.startFrame]));
      if (!e.shiftKey) collapseRef.current = item.id; // plain click without movement narrows the selection
    }
    const next: DragState = {
      kind,
      itemId: item.id,
      pointerId: e.pointerId,
      grabOffsetFrames: kind === 'move' ? frameFromClientX(e.clientX) - item.startFrame : 0,
      origStart: item.startFrame,
      origTrackId: item.trackId,
      ghostStart: item.startFrame,
      ghostTrackId: item.trackId,
      groupIds,
      groupOrigStarts,
      guideFrame: null,
    };
    dragRef.current = next;
    setDrag(next);
  };

  // ---- ruler scrubbing ----
  const onRulerPointerDown = (e: React.PointerEvent) => {
    if (e.button !== 0) return;
    setPlaying(false);
    setPlayhead(frameFromClientX(e.clientX));
    const last = { x: e.clientX };
    const seek = () => setPlayhead(frameFromClientX(last.x));
    const onMove = (ev: PointerEvent) => {
      last.x = ev.clientX;
      seek();
    };
    const scrollEl = scrollRef.current;
    const stopScroll = scrollEl ? startEdgeScroll(scrollEl, () => last.x, seek) : () => {};
    const end = () => {
      stopScroll();
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', end);
      window.removeEventListener('pointercancel', end);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', end);
    window.addEventListener('pointercancel', end);
  };

  // ---- empty lane space: click seeks + clears selection, drag draws a marquee ----
  const onLanePointerDown = (e: React.PointerEvent) => {
    if (e.button !== 0) return;
    const lanes = lanesRef.current;
    if (!lanes) return;
    const startRect = lanes.getBoundingClientRect();
    const x0 = e.clientX - startRect.left;
    const y0 = e.clientY - startRect.top;
    const additive = e.shiftKey;
    const baseSelection = additive ? useEditor.getState().selection : [];
    const last = { x: e.clientX, y: e.clientY };
    let moved = false;
    const startClientX = e.clientX;
    const startClientY = e.clientY;

    const update = () => {
      const rect = lanes.getBoundingClientRect();
      const current = docRef.current;
      if (!current) return;
      const m: Marquee = { x1: x0, y1: y0, x2: last.x - rect.left, y2: last.y - rect.top };
      setMarquee(m);
      const lay = trackLayout(current.tracks.map((t) => t.kind));
      const rowOf = new Map(current.tracks.map((t, i) => [t.id, i]));
      const px = useEditor.getState().pxPerFrame;
      const hits = marqueeHits(
        current.items.map((i) => ({ id: i.id, rowIndex: rowOf.get(i.trackId) ?? 0, startFrame: i.startFrame, endFrame: itemEnd(i) })),
        lay,
        { x1: m.x1 - HEADER_W, y1: m.y1, x2: m.x2 - HEADER_W, y2: m.y2 },
        px,
      );
      useEditor.getState().setSelection(additive ? [...new Set([...baseSelection, ...hits])] : hits);
    };
    const onMove = (ev: PointerEvent) => {
      last.x = ev.clientX;
      last.y = ev.clientY;
      if (!moved && Math.hypot(ev.clientX - startClientX, ev.clientY - startClientY) < 4) return;
      moved = true;
      update();
    };
    const scrollEl = scrollRef.current;
    const stopScroll = scrollEl ? startEdgeScroll(scrollEl, () => last.x, () => moved && update()) : () => {};
    const cleanup = () => {
      stopScroll();
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
      window.removeEventListener('pointercancel', onCancel);
      setMarquee(null);
    };
    const onUp = (ev: PointerEvent) => {
      if (!moved) {
        setPlaying(false);
        setPlayhead(frameFromClientX(ev.clientX));
        if (!additive) useEditor.getState().select(null);
      }
      cleanup();
    };
    const onCancel = () => cleanup();
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp);
    window.addEventListener('pointercancel', onCancel);
  };

  // ---- splitter between stage and timeline ----
  const onSplitterDown = (e: React.PointerEvent) => {
    e.preventDefault();
    const startY = e.clientY;
    const startH = rootRef.current?.getBoundingClientRect().height ?? height;
    const onMove = (ev: PointerEvent) => setUserHeight(Math.round(startH + (startY - ev.clientY)));
    const end = () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', end);
      window.removeEventListener('pointercancel', end);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', end);
    window.addEventListener('pointercancel', end);
  };

  const dragItem = drag ? doc.items.find((i) => i.id === drag.itemId) : undefined;
  const hoverTrack = drag?.hoverTrackId ? doc.tracks.find((t) => t.id === drag.hoverTrackId) : undefined;
  const invalidHover =
    drag?.kind === 'move' && dragItem && hoverTrack ? !trackAllowsItem(hoverTrack.kind, dragItem.type) || hoverTrack.locked : false;
  const dragCursor = !drag ? null : drag.kind !== 'move' ? 'ew-resize' : invalidHover ? 'not-allowed' : 'grabbing';

  const zoomValue = (Math.log(pxPerFrame) - LOG_MIN) / (LOG_MAX - LOG_MIN);
  const zoomBy = (factor: number) => setPxPerFrame(pxPerFrame * factor);
  const interval = rulerInterval(pxPerFrame, fps);
  const subdivisions = rulerSubdivisions(interval, pxPerFrame);
  const ticks: number[] = [];
  for (let f = 0; f <= contentFrames; f += interval) ticks.push(f);
  const minorStep = subdivisions > 0 ? interval / subdivisions : 0;

  return (
    <section
      ref={rootRef}
      aria-label="Timeline"
      className="relative shrink-0 flex flex-col border-t border-line bg-surface-900"
      style={{ height }}
    >
      {dragCursor && <style>{`html, html * { cursor: ${dragCursor} !important; }`}</style>}
      <div
        role="separator"
        aria-orientation="horizontal"
        aria-label="Resize timeline"
        className="absolute -top-1 inset-x-0 h-2 z-50 cursor-row-resize hover:bg-accent/30"
        onPointerDown={onSplitterDown}
        onDoubleClick={() => setUserHeight(null)}
      />

      {/* toolbar */}
      <div className="shrink-0 flex items-center gap-1 px-3" style={{ height: TOOLBAR_H }}>
        <button
          className="icon-btn-sm"
          aria-label="Split at playhead (S)"
          title="Split at playhead (S)"
          onClick={() => void useEditor.getState().splitAtPlayhead()}
        >
          <Icon name="split" />
        </button>
        <button
          className="icon-btn-sm"
          aria-label={`Delete (${deleteKeyLabel(platform)})`}
          title={`Delete (${deleteKeyLabel(platform)})`}
          disabled={selection.length === 0}
          onClick={() => void useEditor.getState().deleteSelection()}
        >
          <Icon name="trash" />
        </button>
        <button
          className="icon-btn-sm"
          aria-label={`Clone (${shortcutLabel('D', platform)})`}
          title={`Clone (${shortcutLabel('D', platform)})`}
          disabled={selection.length === 0}
          onClick={() => void useEditor.getState().cloneSelection()}
        >
          <Icon name="clone" />
        </button>
        <span className="w-px h-[18px] bg-surface-700 mx-1.5" />
        <ToggleButton icon="magnet" label="Snap" active={snapEnabled} onClick={toggleSnap} title="Snap to clip edges, markers and the playhead (N)" />
        <ToggleButton icon="ripple" label="Ripple" active={rippleEnabled} onClick={toggleRipple} title="Ripple delete and trims close the gap" />
        <div className="flex-1" />
        <span className="font-mono text-[11px] text-fg-muted mr-3 tabular-nums" aria-label="Playhead time">
          {formatTimecode(Math.round(playhead), fps)}
        </span>
        <button className="icon-btn-sm !w-7 !h-7" aria-label="Zoom out" title="Zoom out" onClick={() => zoomBy(1 / 1.25)}>
          <Icon name="minus" size={14} />
        </button>
        <input
          type="range"
          min={0}
          max={1}
          step={0.001}
          value={Number.isFinite(zoomValue) ? zoomValue : 0.5}
          aria-label="Timeline zoom"
          onChange={(e) => setPxPerFrame(Math.exp(LOG_MIN + Number(e.target.value) * (LOG_MAX - LOG_MIN)))}
          className="w-[120px] h-1 appearance-none rounded-full bg-surface-700 outline-none cursor-pointer
            [&::-webkit-slider-thumb]:appearance-none [&::-webkit-slider-thumb]:w-3.5 [&::-webkit-slider-thumb]:h-3.5
            [&::-webkit-slider-thumb]:rounded-full [&::-webkit-slider-thumb]:bg-fg"
        />
        <button className="icon-btn-sm !w-7 !h-7" aria-label="Zoom in" title="Zoom in" onClick={() => zoomBy(1.25)}>
          <Icon name="plus" size={14} />
        </button>
        <button className="btn-ghost h-7 px-2.5 text-xs" title="Zoom to fit (Shift+Z)" onClick={() => zoomFit(Math.max(1, total))}>
          Fit
        </button>
      </div>

      {/* scrollable timeline */}
      <div ref={scrollRef} data-timeline-viewport className="flex-1 min-h-0 overflow-auto relative">
        <div className="relative" style={{ width: HEADER_W + contentFrames * pxPerFrame, minWidth: '100%' }}>
          {/* ruler */}
          <div className="sticky top-0 z-30 flex bg-surface-900 border-b border-line" style={{ height: RULER_H }}>
            <div className="sticky left-0 z-30 shrink-0 bg-surface-900" style={{ width: HEADER_W }} />
            <div className="relative flex-1 cursor-ew-resize select-none" onPointerDown={onRulerPointerDown}>
              {ticks.map((f) => (
                <div key={f} className="absolute top-0 bottom-0 border-l border-surface-700/70 pointer-events-none" style={{ left: f * pxPerFrame }}>
                  <span className="absolute top-1.5 left-[9px] text-[11px] leading-none font-mono text-fg-faint whitespace-nowrap">
                    {formatTimecode(f, fps)}
                  </span>
                  {subdivisions > 0 &&
                    Array.from({ length: subdivisions - 1 }, (_, i) => (
                      <span
                        key={i}
                        className="absolute bottom-0 w-px h-1.5 bg-surface-700"
                        style={{ left: (i + 1) * minorStep * pxPerFrame }}
                      />
                    ))}
                </div>
              ))}
              <div
                className="absolute top-0 bottom-0 w-0.5 -ml-px bg-accent pointer-events-none z-10"
                style={{ left: playhead * pxPerFrame }}
              >
                <span className="absolute -left-1.5 top-0 w-[14px] h-3.5 rounded-t-[3px] rounded-b-[7px] bg-accent" />
              </div>
            </div>
          </div>

          {/* lanes */}
          <div ref={lanesRef} className="relative">
            {doc.tracks.map((track, rowIdx) => {
              const rowH = layout[rowIdx]!.height;
              const incompatibleHere = invalidHover && drag?.hoverTrackId === track.id;
              return (
                <div key={track.id} className="flex border-b border-line/60" style={{ height: rowH }}>
                  <TrackGutter track={track} />
                  <div
                    className={`relative flex-1 ${track.locked ? 'bg-surface-850/40' : ''}`}
                    style={incompatibleHere ? { cursor: 'not-allowed' } : undefined}
                    onPointerDown={onLanePointerDown}
                    onDragOver={(e) => {
                      e.preventDefault();
                      e.dataTransfer.dropEffect = 'copy';
                    }}
                    onDrop={(e) => {
                      e.preventDefault();
                      const assetId = e.dataTransfer.getData('text/cutboard-asset');
                      if (assetId) void addAssetToTimeline(assetId, frameFromClientX(e.clientX), track.id);
                    }}
                  >
                    {doc.items
                      .filter((i) => i.trackId === track.id || (drag?.kind === 'move' && drag.itemId === i.id && drag.ghostTrackId === track.id))
                      .map((item) => {
                        const isPrimary = drag?.itemId === item.id;
                        const isGroupMember = !!drag?.groupOrigStarts && item.id in drag.groupOrigStarts && !isPrimary;
                        const moving = drag?.kind === 'move';
                        const inThisLane = isPrimary && moving ? drag.ghostTrackId === track.id : item.trackId === track.id;
                        if (!inThisLane) return null;
                        let start = item.startFrame;
                        let duration = item.durationFrames;
                        if (drag && moving && isPrimary) start = drag.ghostStart;
                        else if (drag && moving && isGroupMember) start = Math.max(0, drag.groupOrigStarts![item.id]! + drag.ghostStart - drag.origStart);
                        else if (drag && isPrimary && drag.ghostFrame !== undefined) {
                          const end = itemEnd(item);
                          if (drag.kind === 'trim-in') {
                            start = Math.min(drag.ghostFrame, end - 1);
                            duration = end - start;
                          } else {
                            duration = Math.max(1, drag.ghostFrame - item.startFrame);
                          }
                        }
                        return (
                          <ItemBlock
                            key={item.id}
                            item={item}
                            rowHeight={rowH}
                            startFrame={start}
                            durationFrames={duration}
                            selected={selection.includes(item.id)}
                            locked={track.locked}
                            lifted={isPrimary || isGroupMember}
                            trimming={isPrimary && drag?.kind !== 'move' && drag?.ghostFrame !== undefined}
                            pxPerFrame={pxPerFrame}
                            fps={fps}
                            onPointerDown={(e, kind) => startDrag(e, item, kind)}
                          />
                        );
                      })}
                  </div>
                </div>
              );
            })}

            {/* marquee */}
            {marquee && (
              <div
                className="absolute z-20 pointer-events-none rounded-[3px] border border-accent bg-accent/10"
                style={{
                  left: Math.min(marquee.x1, marquee.x2),
                  top: Math.min(marquee.y1, marquee.y2),
                  width: Math.abs(marquee.x2 - marquee.x1),
                  height: Math.abs(marquee.y2 - marquee.y1),
                }}
              />
            )}

            {/* snap guide */}
            {drag?.guideFrame != null && (
              <div
                className="absolute top-0 bottom-0 w-px bg-fg/70 pointer-events-none z-20"
                style={{ left: HEADER_W + drag.guideFrame * pxPerFrame }}
                data-testid="snap-guide"
              />
            )}

            {/* playhead */}
            <div
              className="absolute top-0 bottom-0 w-0.5 -ml-px bg-accent pointer-events-none z-10"
              style={{ left: HEADER_W + playhead * pxPerFrame }}
            />
          </div>
        </div>
      </div>
    </section>
  );
}

function ToggleButton({
  icon,
  label,
  active,
  onClick,
  title,
}: {
  icon: IconName;
  label: string;
  active: boolean;
  onClick: () => void;
  title: string;
}) {
  return (
    <button
      type="button"
      aria-pressed={active}
      aria-label={`${label}: ${active ? 'on' : 'off'}`}
      title={title}
      onClick={onClick}
      className={`h-7 px-2.5 rounded-lg inline-flex items-center gap-1.5 text-xs transition-colors ${
        active ? 'bg-surface-800 text-fg' : 'text-fg-faint hover:bg-surface-800/60 hover:text-fg-2'
      }`}
    >
      <Icon name={icon} size={14} />
      {label}
      <span className={`w-1.5 h-1.5 rounded-full ${active ? 'bg-accent' : 'border border-fg-faint'}`} aria-hidden="true" />
    </button>
  );
}

const KIND_ICON: Record<string, IconName> = { video: 'media', audio: 'audio', text: 'text', overlay: 'layers' };

function TrackGutter({ track }: { track: Track }) {
  const applyOps = useEditor((s) => s.applyOps);
  const toggle = (patch: Partial<Pick<Track, 'muted' | 'hidden' | 'locked'>>, label: string) =>
    void applyOps([{ type: 'track.update', trackId: track.id, patch }], label);
  const btn = (active: boolean) =>
    `w-6 h-6 rounded-md inline-flex items-center justify-center transition-colors ${
      active ? 'bg-surface-700 text-fg' : 'text-fg-muted hover:bg-surface-800 hover:text-fg'
    }`;
  return (
    <div
      className="group sticky left-0 z-30 shrink-0 bg-surface-900 border-r border-line flex flex-col items-center justify-center gap-0.5 text-fg-faint"
      style={{ width: HEADER_W }}
      title={track.name}
    >
      <Icon name={KIND_ICON[track.kind] ?? 'layers'} size={15} />
      {(track.locked || track.muted || track.hidden) && (
        <span className="flex items-center gap-0.5 text-fg-muted">
          {track.locked && <Icon name="lock" size={11} />}
          {track.muted && <Icon name="mute" size={11} />}
          {track.hidden && <Icon name="eyeOff" size={11} />}
        </span>
      )}
      <div
        className="absolute left-full top-1/2 -translate-y-1/2 ml-1 z-40 flex gap-0.5 p-0.5 rounded-lg border border-surface-700 bg-surface-850 shadow-lg
          opacity-0 pointer-events-none group-hover:opacity-100 group-hover:pointer-events-auto group-focus-within:opacity-100 group-focus-within:pointer-events-auto"
      >
        <button
          className={btn(track.muted)}
          aria-pressed={track.muted}
          aria-label={`Mute ${track.name}`}
          title={track.muted ? 'Unmute' : 'Mute'}
          onClick={() => toggle({ muted: !track.muted }, 'Toggle mute')}
        >
          <Icon name={track.muted ? 'mute' : 'volume'} size={14} />
        </button>
        <button
          className={btn(track.hidden)}
          aria-pressed={track.hidden}
          aria-label={`Hide ${track.name}`}
          title={track.hidden ? 'Show' : 'Hide'}
          onClick={() => toggle({ hidden: !track.hidden }, 'Toggle visibility')}
        >
          <Icon name={track.hidden ? 'eyeOff' : 'eye'} size={14} />
        </button>
        <button
          className={btn(track.locked)}
          aria-pressed={track.locked}
          aria-label={`Lock ${track.name}`}
          title={track.locked ? 'Unlock' : 'Lock'}
          onClick={() => toggle({ locked: !track.locked }, 'Toggle lock')}
        >
          <Icon name={track.locked ? 'lock' : 'unlock'} size={14} />
        </button>
      </div>
    </div>
  );
}

const TYPE_BG: Record<string, string> = {
  video: 'bg-[#1d2733]',
  image: 'bg-[#2a2336]',
  audio: 'bg-[#12261f]',
  text: 'bg-[#2a2540]',
  caption: 'bg-[#2a2540]',
  shape: 'bg-[#33271f]',
  motionGraphic: 'bg-[#1f2540]',
};

function ItemBlock({
  item,
  rowHeight,
  startFrame,
  durationFrames,
  selected,
  locked,
  lifted,
  trimming,
  pxPerFrame,
  fps,
  onPointerDown,
}: {
  item: Item;
  rowHeight: number;
  /** position/size to draw at (differs from the item while it is being dragged or trimmed) */
  startFrame: number;
  durationFrames: number;
  selected: boolean;
  locked: boolean;
  lifted: boolean;
  trimming: boolean;
  pxPerFrame: number;
  fps: number;
  onPointerDown: (e: React.PointerEvent, kind: DragState['kind']) => void;
}) {
  const asset = useEditor((s) => s.assets.find((a) => a.id === item.assetId));
  const width = Math.max(4, durationFrames * pxPerFrame);
  const left = startFrame * pxPerFrame;
  const hw = trimHandleWidth(width);
  const thumb = asset?.thumbPath && (item.type === 'video' || item.type === 'image') ? mediaUrl(asset.thumbPath) : null;
  const waveform = item.type === 'audio' && asset?.waveformPath ? mediaUrl(asset.waveformPath) : null;
  const label = itemLabel(item, asset?.originalName);
  const keyframeCount = item.keyframes ? Object.keys(item.keyframes).length : 0;

  return (
    <div
      className={`group/clip absolute rounded-lg select-none ${TYPE_BG[item.type] ?? TYPE_BG.video} ${
        locked ? 'cursor-default' : 'cursor-grab'
      } ${selected ? 'outline outline-[1.5px] outline-accent z-[5]' : ''} ${lifted ? 'z-[6] opacity-90 shadow-lg shadow-black/50' : ''} ${
        trimming ? 'opacity-70' : ''
      }`}
      style={{ left, width, top: ROW_PAD, height: rowHeight - ROW_PAD * 2 }}
      onPointerDown={(e) => onPointerDown(e, 'move')}
      title={`${label} · ${formatTimecode(startFrame, fps)} → ${formatTimecode(startFrame + durationFrames, fps)}`}
    >
      <div className="absolute inset-0 rounded-lg overflow-hidden shadow-[inset_0_0_0_1px_rgba(255,255,255,0.06)]">
        {thumb && (
          <div
            className="absolute inset-0 opacity-80"
            style={{ backgroundImage: `url(${thumb})`, backgroundRepeat: 'repeat-x', backgroundSize: 'auto 100%', backgroundPosition: 'left center' }}
          />
        )}
        {waveform && (
          <div
            className="absolute inset-0 opacity-70"
            style={{ backgroundImage: `url(${waveform})`, backgroundRepeat: 'no-repeat', backgroundSize: '100% 100%' }}
          />
        )}
        {width > 36 && (
          <span className="absolute left-1.5 top-1.5 max-w-[calc(100%-12px)] truncate px-1.5 py-px rounded-[5px] bg-surface-950/60 text-[11px] leading-4 text-fg">
            {label}
          </span>
        )}
        {keyframeCount > 0 && width > 48 && (
          <span
            className="absolute bottom-1 right-1.5 inline-flex items-center gap-1 px-1 rounded bg-surface-950/60 font-mono text-[11px] text-fg-2"
            title={`${keyframeCount} keyframed propert${keyframeCount > 1 ? 'ies' : 'y'}`}
          >
            <span className="w-1.5 h-1.5 rotate-45 bg-fg-2" aria-hidden="true" />
            {keyframeCount}
          </span>
        )}
      </div>

      {/* trim handles: invisible hit areas with an edge highlight on hover */}
      {hw > 0 && !locked && (
        <>
          <div
            className="group/h absolute left-0 top-0 bottom-0 cursor-w-resize flex items-stretch justify-start"
            style={{ width: hw }}
            onPointerDown={(e) => onPointerDown(e, 'trim-in')}
          >
            <span className="w-[3px] rounded-l-lg bg-fg/0 group-hover/h:bg-fg/70 transition-colors" />
          </div>
          <div
            className="group/h absolute right-0 top-0 bottom-0 cursor-e-resize flex items-stretch justify-end"
            style={{ width: hw }}
            onPointerDown={(e) => onPointerDown(e, 'trim-out')}
          >
            <span className="w-[3px] rounded-r-lg bg-fg/0 group-hover/h:bg-fg/70 transition-colors" />
          </div>
        </>
      )}

      {trimming && (
        <span className="absolute left-full top-1/2 -translate-y-1/2 ml-2 z-40 whitespace-nowrap px-1.5 py-0.5 rounded-md bg-surface-950 border border-surface-700 font-mono text-[11px] text-fg">
          {formatTimecode(startFrame, fps)} · {formatDurationBadge(durationFrames, fps)}
        </span>
      )}
    </div>
  );
}
