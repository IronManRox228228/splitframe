import { useCallback, useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Item, Track, formatTimecode } from '@cutboard/schema';
import { docDurationFrames, itemEnd, trackAllowsItem, getSnapCandidates, snapFrame } from '@cutboard/editor-core';
import { mediaUrl } from '../lib/url.ts';
import { HEADER_W, dragCommit, frameFromPointer, type DragState } from '../lib/timeline-math.ts';
import { deleteKeyLabel, shortcutLabel } from '../lib/platform-labels.ts';

const ROW_H = 56;

export function Timeline() {
  const doc = useEditor((s) => s.doc);
  const playhead = useEditor((s) => s.playhead);
  const pxPerFrame = useEditor((s) => s.pxPerFrame);
  const setPlayhead = useEditor((s) => s.setPlayhead);
  const setPlaying = useEditor((s) => s.setPlaying);
  const selection = useEditor((s) => s.selection);
  const select = useEditor((s) => s.select);
  const applyOps = useEditor((s) => s.applyOps);
  const snapEnabled = useEditor((s) => s.snapEnabled);
  const rippleEnabled = useEditor((s) => s.rippleEnabled);
  const toggleSnap = useEditor((s) => s.toggleSnap);
  const toggleRipple = useEditor((s) => s.toggleRipple);
  const setPxPerFrame = useEditor((s) => s.setPxPerFrame);
  const zoomFit = useEditor((s) => s.zoomFit);
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);
  const platform = useEditor((s) => s.appInfo?.platform);

  const lanesRef = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<DragState | null>(null);
  // the pointerup handler is registered once per drag; it reads the live state from here
  const dragRef = useRef<DragState | null>(null);

  const frameFromClientX = useCallback(
    (clientX: number): number => {
      const el = lanesRef.current;
      if (!el) return 0;
      return frameFromPointer(clientX, el.getBoundingClientRect().left, pxPerFrame);
    },
    [pxPerFrame],
  );

  const trackAtClientY = useCallback(
    (clientY: number): Track | null => {
      const el = lanesRef.current;
      if (!el || !doc) return null;
      const rect = el.getBoundingClientRect();
      const idx = Math.floor((clientY - rect.top) / ROW_H);
      const track = doc.tracks[Math.max(0, Math.min(doc.tracks.length - 1, idx))];
      return track ?? null;
    },
    [doc],
  );

  // pointermove/up for drags (window-level so fast drags don't lose the pointer)
  useEffect(() => {
    if (!drag || !doc) return;
    const item = doc.items.find((i) => i.id === drag.itemId);
    if (!item) return;

    const onMove = (e: PointerEvent) => {
      const state = useEditor.getState();
      const frame = frameFromClientX(e.clientX);
      let next: Partial<DragState> = {};
      if (drag.kind === 'move') {
        const rawStart = Math.max(0, frame - drag.grabOffsetFrames);
        let start = rawStart;
        if (state.snapEnabled) {
          const threshold = Math.round(8 / state.pxPerFrame);
          start = snapBothEdges(doc, item, rawStart, threshold);
        }
        const track = trackAtClientY(e.clientY);
        const targetTrack =
          track && trackAllowsItem(track.kind, item.type) && !track.locked ? track : doc.tracks.find((t) => t.id === drag.origTrackId)!;
        next = { ghostStart: start, ghostTrackId: targetTrack.id };
      } else {
        next = { ghostFrame: frame };
      }
      if (dragRef.current) dragRef.current = { ...dragRef.current, ...next } as DragState;
      setDrag((d) => (d ? { ...d, ...next } as DragState : d));
    };
    const onUp = () => {
      const final = dragRef.current;
      const commit = final ? dragCommit(final, useEditor.getState().rippleEnabled) : null;
      if (commit) void applyOps(commit.ops, commit.label);
      dragRef.current = null;
      setDrag(null);
    };
    window.addEventListener('pointermove', onMove);
    window.addEventListener('pointerup', onUp, { once: true });
    return () => {
      window.removeEventListener('pointermove', onMove);
      window.removeEventListener('pointerup', onUp);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [drag?.itemId, drag?.kind, drag?.pointerId]);

  if (!doc) return null;
  const total = docDurationFrames(doc);
  const contentFrames = Math.max(total + Math.round(300 / pxPerFrame), 120);

  const startDrag = (e: React.PointerEvent, item: Item, kind: DragState['kind']) => {
    const track = doc.tracks.find((t) => t.id === item.trackId);
    if (track?.locked) return;
    e.stopPropagation();
    select(item.id, e.shiftKey);
    setPlaying(false);
    const next: DragState = {
      kind,
      itemId: item.id,
      pointerId: e.pointerId,
      grabOffsetFrames: kind === 'move' ? frameFromClientX(e.clientX) - item.startFrame : 0,
      origStart: item.startFrame,
      origTrackId: item.trackId,
      ghostStart: item.startFrame,
      ghostTrackId: item.trackId,
    };
    dragRef.current = next;
    setDrag(next);
  };

  return (
    <div className="h-[300px] shrink-0 flex flex-col border-t border-line bg-surface-950">
      {/* toolbar */}
      <div className="h-9 shrink-0 flex items-center gap-1 px-3 border-b border-line bg-surface-900">
        <ToolButton label="Delete" onClick={() => void useEditor.getState().deleteSelection()} disabled={selection.length === 0} title={`Delete (${deleteKeyLabel(platform)})`} />
        <ToolButton label="Split" onClick={() => void useEditor.getState().splitAtPlayhead()} title="Split at playhead (S)" />
        <ToolButton label="Clone" onClick={() => void useEditor.getState().cloneSelection()} disabled={selection.length === 0} title={`Clone (${shortcutLabel('D', platform)})`} />
        <div className="w-px h-4 bg-line mx-1" />
        <ToggleChip active={rippleEnabled} label="Ripple" onClick={toggleRipple} title="Ripple delete & trims" />
        <ToggleChip active={snapEnabled} label="Snap" onClick={toggleSnap} title="Snap to edges & markers" />
        <div className="flex-1" />
        <span className="text-[11px] text-neutral-600 font-mono mr-2">
          {formatTimecode(Math.round(playhead), doc.project.fps)}
        </span>
        <ToolButton label="−" onClick={() => setPxPerFrame(pxPerFrame / 1.25)} title="Zoom out" />
        <ToolButton label="+" onClick={() => setPxPerFrame(pxPerFrame * 1.25)} title="Zoom in" />
        <ToolButton label="Fit" onClick={() => zoomFit(Math.max(1, total))} title="Zoom to fit (Shift+Z)" />
      </div>

      {/* scrollable timeline */}
      <div className="flex-1 overflow-auto relative" onPointerDown={() => select(null)}>
        <div style={{ width: HEADER_W + contentFrames * pxPerFrame, minWidth: '100%' }}>
          {/* ruler */}
          <div className="sticky top-0 z-20 flex h-7 bg-surface-900/95 backdrop-blur border-b border-line">
            <div
              className="sticky left-0 z-30 shrink-0 bg-surface-900 border-r border-line flex items-center px-2 text-[10px] text-neutral-600"
              style={{ width: HEADER_W }}
            >
              {doc.project.width}×{doc.project.height}
            </div>
            <div
              className="relative flex-1 cursor-ew-resize"
              onPointerDown={(e) => {
                setPlaying(false);
                setPlayhead(frameFromClientX(e.clientX));
                const onMove = (ev: PointerEvent) => setPlayhead(frameFromClientX(ev.clientX));
                const onUp = () => {
                  window.removeEventListener('pointermove', onMove);
                };
                window.addEventListener('pointermove', onMove);
                window.addEventListener('pointerup', onUp, { once: true });
              }}
            >
              <RulerTicks contentFrames={contentFrames} pxPerFrame={pxPerFrame} fps={doc.project.fps} />
            </div>
          </div>

          {/* tracks */}
          <div ref={lanesRef} className="relative">
            {doc.tracks.map((track) => (
              <div key={track.id} className="flex border-b border-line" style={{ height: ROW_H }}>
                <TrackHeader track={track} />
                <div
                  className="relative flex-1 bg-surface-950"
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
                    .filter((i) => i.trackId === track.id)
                    .map((item) => {
                      const isDragging = drag?.itemId === item.id;
                      const startFrame = isDragging && drag.kind === 'move' ? drag.ghostStart : item.startFrame;
                      const trackId = isDragging && drag.kind === 'move' ? drag.ghostTrackId : item.trackId;
                      if (trackId !== track.id) return null;
                      return (
                        <ItemBlock
                          key={item.id}
                          item={item}
                          startFrame={startFrame}
                          selected={selection.includes(item.id)}
                          pxPerFrame={pxPerFrame}
                          fps={doc.project.fps}
                          ghostFrame={isDragging && drag.kind !== 'move' ? drag.ghostFrame : undefined}
                          onPointerDown={(e, kind) => startDrag(e, item, kind)}
                        />
                      );
                    })}
                </div>
              </div>
            ))}
            {/* playhead */}
            <div
              className="absolute top-0 bottom-0 w-px bg-accent pointer-events-none z-10"
              style={{ left: HEADER_W + playhead * pxPerFrame }}
            >
              <div className="w-2 h-2.5 -ml-[3.5px] bg-accent rounded-b-sm" />
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

function snapBothEdges(doc: NonNullable<ReturnType<typeof useEditor.getState>['doc']>, item: Item, proposedStart: number, threshold: number): number {
  const candidates = getSnapCandidates(doc, { excludeItemIds: [item.id] });
  const inSnap = snapFrame(proposedStart, candidates, threshold);
  const outSnap = snapFrame(proposedStart + item.durationFrames, candidates, threshold);
  if (inSnap && (!outSnap || Math.abs(inSnap.delta) <= Math.abs(outSnap.delta))) return inSnap.frame;
  if (outSnap) return Math.max(0, outSnap.frame - item.durationFrames);
  return proposedStart;
}

function RulerTicks({ contentFrames, pxPerFrame, fps }: { contentFrames: number; pxPerFrame: number; fps: number }) {
  const stepFrames = Math.max(1, Math.round(fps / Math.max(0.5, pxPerFrame * fps / 90) / 1) * 1);
  // choose a tick interval that keeps labels ~90px apart
  const candidates = [1, 5, 10, 15, 30, 60, 150, 300, 600, 1800];
  const interval = candidates.find((f) => f * pxPerFrame >= 70) ?? 3600;
  void stepFrames;
  const ticks: number[] = [];
  for (let f = 0; f <= contentFrames; f += interval) ticks.push(f);
  return (
    <div className="absolute inset-0">
      {ticks.map((f) => (
        <div key={f} className="absolute top-0 bottom-0 border-l border-line/80" style={{ left: f * pxPerFrame }}>
          <span className="absolute top-0.5 left-1 text-[9px] text-neutral-500 font-mono">
            {formatTimecode(f, fps)}
          </span>
        </div>
      ))}
    </div>
  );
}

function TrackHeader({ track }: { track: Track }) {
  const applyOps = useEditor((s) => s.applyOps);
  return (
    <div
      className="sticky left-0 z-20 shrink-0 bg-surface-900 border-r border-line flex items-center gap-1.5 px-2"
      style={{ width: HEADER_W }}
    >
      <span
        className={`w-1.5 h-1.5 rounded-full shrink-0 ${
          track.kind === 'video' ? 'bg-sky-500' : track.kind === 'audio' ? 'bg-emerald-500' : track.kind === 'text' ? 'bg-fuchsia-500' : 'bg-orange-400'
        }`}
      />
      <span className="text-[11px] text-neutral-300 truncate flex-1">{track.name}</span>
      <button
        className={`w-5 h-5 rounded text-[10px] ${track.muted ? 'bg-red-500/20 text-red-400' : 'text-neutral-600 hover:text-neutral-300'}`}
        title={track.muted ? 'Unmute' : 'Mute'}
        onClick={() => void applyOps([{ type: 'track.update', trackId: track.id, patch: { muted: !track.muted } }], 'Toggle mute')}
      >
        M
      </button>
      <button
        className={`w-5 h-5 rounded text-[10px] ${track.hidden ? 'bg-amber-500/20 text-amber-400' : 'text-neutral-600 hover:text-neutral-300'}`}
        title={track.hidden ? 'Show' : 'Hide'}
        onClick={() => void applyOps([{ type: 'track.update', trackId: track.id, patch: { hidden: !track.hidden } }], 'Toggle visibility')}
      >
        👁
      </button>
      <button
        className={`w-5 h-5 rounded text-[10px] ${track.locked ? 'bg-neutral-500/25 text-neutral-300' : 'text-neutral-600 hover:text-neutral-300'}`}
        title={track.locked ? 'Unlock' : 'Lock'}
        onClick={() => void applyOps([{ type: 'track.update', trackId: track.id, patch: { locked: !track.locked } }], 'Toggle lock')}
      >
        {track.locked ? '🔒' : '🔓'}
      </button>
    </div>
  );
}

const TYPE_COLORS: Record<string, { bg: string; border: string }> = {
  video: { bg: 'bg-sky-600/35', border: 'border-sky-500/60' },
  audio: { bg: 'bg-emerald-600/30', border: 'border-emerald-500/60' },
  image: { bg: 'bg-violet-600/30', border: 'border-violet-500/60' },
  text: { bg: 'bg-fuchsia-600/30', border: 'border-fuchsia-500/60' },
  caption: { bg: 'bg-fuchsia-600/30', border: 'border-fuchsia-500/60' },
  shape: { bg: 'bg-orange-600/30', border: 'border-orange-500/60' },
  motionGraphic: { bg: 'bg-blue-600/30', border: 'border-blue-500/60' },
};

function ItemBlock({
  item,
  startFrame,
  selected,
  pxPerFrame,
  fps,
  ghostFrame,
  onPointerDown,
}: {
  item: Item;
  /** position to draw at (differs from item.startFrame while the clip is being dragged) */
  startFrame: number;
  selected: boolean;
  pxPerFrame: number;
  fps: number;
  ghostFrame?: number;
  onPointerDown: (e: React.PointerEvent, kind: DragState['kind']) => void;
}) {
  const asset = useEditor((s) => s.assets.find((a) => a.id === item.assetId));
  const width = Math.max(6, item.durationFrames * pxPerFrame);
  const left = startFrame * pxPerFrame;
  const colors = TYPE_COLORS[item.type] ?? TYPE_COLORS.video!;
  const thumb = asset?.thumbPath && (item.type === 'video' || item.type === 'image') ? mediaUrl(asset.thumbPath) : null;

  return (
    <div
      className={`absolute top-1 bottom-1 rounded-md border overflow-hidden group cursor-grab active:cursor-grabbing ${
        colors.bg
      } ${selected ? 'border-accent ring-1 ring-accent' : colors.border}`}
      style={{ left, width }}
      onPointerDown={(e) => onPointerDown(e, 'move')}
      title={`${item.type} · ${formatTimecode(item.startFrame, fps)} → ${formatTimecode(itemEnd(item), fps)}`}
    >
      {thumb && (
        <div
          className="absolute inset-0 opacity-35 bg-cover bg-center"
          style={{ backgroundImage: `url(${thumb})` }}
        />
      )}
      {item.type === 'audio' && asset?.waveformPath && (
        <div className="absolute inset-0 opacity-50 bg-contain bg-center bg-no-repeat" style={{ backgroundImage: `url(${mediaUrl(asset.waveformPath)})` }} />
      )}
      <div className="absolute inset-x-0 top-0 px-1.5 py-0.5 flex items-center gap-1">
        <span className="text-[9px] uppercase tracking-wide text-white/80 truncate">
          {item.labels?.name ?? item.type}
        </span>
      </div>
      {item.keyframes && Object.keys(item.keyframes).length > 0 && (
        <span className="absolute bottom-0.5 right-1 text-[8px] text-white/70">◆ {Object.keys(item.keyframes).length}</span>
      )}
      {/* trim handles */}
      <div
        className="absolute left-0 top-0 bottom-0 w-1.5 cursor-w-resize opacity-0 group-hover:opacity-100 bg-white/30"
        onPointerDown={(e) => onPointerDown(e, 'trim-in')}
      />
      <div
        className="absolute right-0 top-0 bottom-0 w-1.5 cursor-e-resize opacity-0 group-hover:opacity-100 bg-white/30"
        onPointerDown={(e) => onPointerDown(e, 'trim-out')}
      />
      {ghostFrame !== undefined && (
        <div
          className="absolute top-0 bottom-0 w-px bg-accent"
          style={{ left: (ghostFrame - startFrame) * pxPerFrame }}
        />
      )}
    </div>
  );
}

function ToolButton({ label, onClick, disabled, title }: { label: string; onClick: () => void; disabled?: boolean; title?: string }) {
  return (
    <button className="btn-ghost disabled:opacity-40 disabled:pointer-events-none" onClick={onClick} disabled={disabled} title={title}>
      {label}
    </button>
  );
}

function ToggleChip({ active, label, onClick, title }: { active: boolean; label: string; onClick: () => void; title?: string }) {
  return (
    <button
      onClick={onClick}
      title={title}
      className={`chip h-6 px-2 border ${
        active ? 'bg-accent/15 text-accent border-accent/40' : 'text-neutral-500 border-line hover:text-neutral-300'
      }`}
    >
      {label}
    </button>
  );
}
