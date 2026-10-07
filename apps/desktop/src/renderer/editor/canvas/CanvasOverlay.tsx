import { useEffect, useMemo, useRef, useState } from 'react';
import type { Item, Op, TimelineDoc } from '@cutboard/schema';
import { hitTestBounds, itemBoundsAt, type ItemBounds } from '@cutboard/renderer';
import { useEditor } from '../../store.ts';
import { measureText } from './measure.ts';
import {
  boxExtent,
  rotateAboutCenter,
  scaleFromCorner,
  snapAxis,
  snapTargets,
  type LiveTransform,
  type Pt,
} from './geometry.ts';

export interface OverlayRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

export type LiveMap = Record<string, LiveTransform & { opacity?: number }>;

interface Props {
  /** the doc as drawn (including any live drag transforms) */
  doc: TimelineDoc;
  frame: number;
  canvasW: number;
  canvasH: number;
  rect: OverlayRect;
  /** bumps when media sizes may have changed */
  sizeTick: number;
  mediaSize: (assetId: string) => { width: number; height: number } | null;
  onLive: (live: LiveMap | null) => void;
}

interface DragItem {
  bounds: ItemBounds;
  transform: Item['transform'];
}

type Drag =
  | { kind: 'move'; start: Pt; items: DragItem[]; moved: boolean; pointerId: number }
  | { kind: 'scale'; start: Pt; item: DragItem; corner: number; moved: boolean; pointerId: number }
  | { kind: 'rotate'; start: Pt; item: DragItem; moved: boolean; pointerId: number };

const DRAG_THRESHOLD_PX = 3;
const SNAP_PX = 6;
const CURSORS = ['ew-resize', 'nwse-resize', 'ns-resize', 'nesw-resize'];

const round2 = (n: number): number => Math.round(n * 100) / 100;

/** Pointer + keyboard editing layer over the preview canvas. DOM only; never drawn into frames. */
export function CanvasOverlay({ doc, frame, canvasW, canvasH, rect, sizeTick, mediaSize, onLive }: Props) {
  const selection = useEditor((s) => s.selection);
  const playing = useEditor((s) => s.playing);
  const surfaceRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<Drag | null>(null);
  const lastLive = useRef<LiveMap | null>(null);
  const [hoverId, setHoverId] = useState<string | null>(null);
  const [guides, setGuides] = useState<{ x: number | null; y: number | null }>({ x: null, y: null });
  const [dragging, setDragging] = useState(false);
  const [editing, setEditing] = useState<string | null>(null);

  const k = rect.width / canvasW;

  const emit = (live: LiveMap | null) => {
    lastLive.current = live;
    onLive(live);
  };

  const bounds = useMemo(
    () => itemBoundsAt(doc, frame, canvasW, canvasH, { measure: measureText, mediaSize }),
    // mediaSize reads a live pool; sizeTick says when it changed
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [doc, frame, canvasW, canvasH, sizeTick],
  );
  const byId = useMemo(() => new Map(bounds.map((b) => [b.itemId, b])), [bounds]);
  const pickable = useMemo(() => bounds.filter((b) => !b.locked), [bounds]);

  const selected = selection.map((id) => byId.get(id)).filter((b): b is ItemBounds => Boolean(b) && !b!.locked);
  const single = selected.length === 1 ? selected[0]! : null;
  const canTransform = Boolean(single && !single.animated);

  // an item leaving the playhead's range ends its text edit
  useEffect(() => {
    if (editing && !byId.has(editing)) setEditing(null);
  }, [byId, editing]);

  const toCanvas = (e: { clientX: number; clientY: number }): Pt => {
    const r = surfaceRef.current!.getBoundingClientRect();
    return { x: ((e.clientX - r.left) / r.width) * canvasW, y: ((e.clientY - r.top) / r.height) * canvasH };
  };

  const hit = (p: Pt): ItemBounds | null => hitTestBounds(pickable, p.x, p.y);
  const itemOf = (id: string): Item | undefined => useEditor.getState().doc?.items.find((i) => i.id === id);

  /** One applyOps call = one undo step, however many items moved. */
  const commit = async (live: LiveMap, label: string) => {
    const ops: Op[] = [];
    for (const [itemId, t] of Object.entries(live)) {
      const item = itemOf(itemId);
      if (!item) continue;
      const patch: Record<string, number> = {};
      for (const key of ['x', 'y', 'scale', 'scaleX', 'scaleY', 'rotation'] as const) {
        const value = t[key];
        if (value === undefined) continue;
        const v = round2(value);
        if (round2(item.transform[key]) !== v) patch[key] = v;
      }
      if (Object.keys(patch).length > 0) ops.push({ type: 'item.update', itemId, patch: { transform: patch } } as Op);
    }
    await useEditor.getState().applyOps(ops, label);
  };

  const beginMove = (p: Pt, pointerId: number, ids: string[]) => {
    const items = ids
      .map((id) => byId.get(id))
      .filter((b): b is ItemBounds => Boolean(b) && !b!.locked && !b!.animated)
      .map((b) => ({ bounds: b, transform: itemOf(b.itemId)!.transform }));
    if (items.length === 0) return;
    dragRef.current = { kind: 'move', start: p, items, moved: false, pointerId };
    surfaceRef.current?.setPointerCapture(pointerId);
  };

  const onPointerDown = (e: React.PointerEvent) => {
    if (e.button !== 0 || playing || editing) return;
    surfaceRef.current?.focus({ preventScroll: true });
    const p = toCanvas(e);
    const target = hit(p);
    const store = useEditor.getState();
    if (!target) {
      if (!e.shiftKey) store.select(null);
      return;
    }
    if (e.shiftKey) {
      store.select(target.itemId, true);
      return;
    }
    const sel = store.selection;
    const inSel = sel.includes(target.itemId);
    if (!inSel) store.select(target.itemId);
    beginMove(p, e.pointerId, inSel ? sel : [target.itemId]);
  };

  const onHandleDown = (e: React.PointerEvent, kind: 'scale' | 'rotate', corner = 0) => {
    if (e.button !== 0 || !single || single.animated) return;
    e.stopPropagation();
    e.preventDefault();
    surfaceRef.current?.focus({ preventScroll: true });
    const item = { bounds: single, transform: itemOf(single.itemId)!.transform };
    const p = toCanvas(e);
    dragRef.current =
      kind === 'scale'
        ? { kind: 'scale', start: p, item, corner, moved: false, pointerId: e.pointerId }
        : { kind: 'rotate', start: p, item, moved: false, pointerId: e.pointerId };
    surfaceRef.current?.setPointerCapture(e.pointerId);
  };

  const onPointerMove = (e: React.PointerEvent) => {
    const drag = dragRef.current;
    const p = toCanvas(e);
    if (!drag) {
      if (playing || editing) return;
      const h = hit(p);
      setHoverId(h ? h.itemId : null);
      return;
    }
    if (!drag.moved) {
      if (Math.hypot(p.x - drag.start.x, p.y - drag.start.y) * k < DRAG_THRESHOLD_PX) return;
      drag.moved = true;
      setDragging(true);
    }
    if (drag.kind === 'move') {
      let dx = p.x - drag.start.x;
      let dy = p.y - drag.start.y;
      let gx: number | null = null;
      let gy: number | null = null;
      if (!e.altKey) {
        const ext = drag.items.map((i) => boxExtent(i.bounds));
        const left = Math.min(...ext.map((x) => x.left));
        const right = Math.max(...ext.map((x) => x.right));
        const top = Math.min(...ext.map((x) => x.top));
        const bottom = Math.max(...ext.map((x) => x.bottom));
        const t = snapTargets(canvasW, canvasH);
        const sx = snapAxis([left + dx, (left + right) / 2 + dx, right + dx], t.xs, SNAP_PX / k);
        const sy = snapAxis([top + dy, (top + bottom) / 2 + dy, bottom + dy], t.ys, SNAP_PX / k);
        if (sx) {
          dx += sx.delta;
          gx = sx.target;
        }
        if (sy) {
          dy += sy.delta;
          gy = sy.target;
        }
      }
      setGuides({ x: gx, y: gy });
      const live: LiveMap = {};
      for (const i of drag.items) live[i.bounds.itemId] = { x: i.transform.x + dx, y: i.transform.y + dy };
      emit(live);
    } else if (drag.kind === 'scale') {
      const type = drag.item.bounds.type;
      const free = e.shiftKey && type !== 'caption' && type !== 'motionGraphic';
      emit({ [drag.item.bounds.itemId]: scaleFromCorner(drag.item.bounds, drag.item.transform, drag.corner, p, free) });
    } else {
      emit({
        [drag.item.bounds.itemId]: rotateAboutCenter(drag.item.bounds, drag.item.transform.rotation, drag.start, p, e.shiftKey ? 15 : null),
      });
    }
  };

  const endDrag = async (cancel: boolean) => {
    const drag = dragRef.current;
    if (!drag) return;
    dragRef.current = null;
    try {
      surfaceRef.current?.releasePointerCapture(drag.pointerId);
    } catch {
      /* already released */
    }
    setDragging(false);
    setGuides({ x: null, y: null });
    const live = lastLive.current;
    lastLive.current = null;
    if (drag.moved && !cancel && live) {
      await commit(live, drag.kind === 'move' ? 'Move' : drag.kind === 'scale' ? 'Scale' : 'Rotate');
    }
    onLive(null);
  };

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (editing) return;
    if (e.key === 'Escape' && dragRef.current) {
      e.preventDefault();
      e.stopPropagation();
      void endDrag(true);
      return;
    }
    const dir: Record<string, Pt> = {
      ArrowLeft: { x: -1, y: 0 },
      ArrowRight: { x: 1, y: 0 },
      ArrowUp: { x: 0, y: -1 },
      ArrowDown: { x: 0, y: 1 },
    };
    const d = dir[e.key];
    if (!d || e.ctrlKey || e.metaKey || e.altKey || dragRef.current) return;
    const targets = selected.filter((b) => !b.animated);
    if (targets.length === 0) return; // nothing to nudge: the timeline's frame-step shortcut runs
    e.preventDefault();
    e.stopPropagation(); // keep the global arrow shortcuts (frame step) from also firing
    const step = e.shiftKey ? 10 : 1;
    const live: LiveMap = {};
    for (const b of targets) {
      const t = itemOf(b.itemId)?.transform;
      if (t) live[b.itemId] = { x: t.x + d.x * step, y: t.y + d.y * step };
    }
    void commit(live, 'Nudge');
  };

  const onDoubleClick = (e: React.MouseEvent) => {
    if (playing) return;
    const h = hit(toCanvas(e));
    if (h && h.type === 'text') {
      useEditor.getState().select(h.itemId);
      setHoverId(null);
      setEditing(h.itemId);
    }
  };

  const hover = hoverId && !dragging && !editing && !selection.includes(hoverId) ? byId.get(hoverId) : null;
  const over = !dragging && hoverId ? byId.get(hoverId) : null;
  const cursor = dragging ? 'grabbing' : over && !over.animated ? 'move' : 'default';
  const editingBounds = editing ? byId.get(editing) : undefined;

  return (
    <div
      ref={surfaceRef}
      tabIndex={0}
      aria-label="Canvas"
      className="absolute outline-none"
      style={{
        left: rect.left,
        top: rect.top,
        width: rect.width,
        height: rect.height,
        cursor,
        touchAction: 'none',
        pointerEvents: playing ? 'none' : 'auto',
      }}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={() => void endDrag(false)}
      onPointerCancel={() => void endDrag(true)}
      onPointerLeave={() => {
        if (!dragRef.current) setHoverId(null);
      }}
      onKeyDown={onKeyDown}
      onDoubleClick={onDoubleClick}
    >
      {hover && <BoxFrame b={hover} k={k} variant="hover" />}
      {selected.map((b) => (
        <BoxFrame key={b.itemId} b={b} k={k} variant="selected">
          {canTransform && single && b.itemId === single.itemId && !editing && <Handles b={b} onDown={onHandleDown} />}
        </BoxFrame>
      ))}
      {guides.x !== null && (
        <div className="absolute top-0 bottom-0 w-px bg-accent pointer-events-none" style={{ left: guides.x * k }} />
      )}
      {guides.y !== null && (
        <div className="absolute left-0 right-0 h-px bg-accent pointer-events-none" style={{ top: guides.y * k }} />
      )}
      {editing && editingBounds && (
        <TextEditor
          b={editingBounds}
          item={itemOf(editing)}
          k={k}
          onStart={() => onLive({ [editing]: { opacity: 0 } })}
          onDone={(text) => {
            const item = itemOf(editing);
            setEditing(null);
            onLive(null);
            surfaceRef.current?.focus({ preventScroll: true });
            if (text !== null && item && text.trim() !== '' && text !== (item.props as { text: string }).text) {
              void useEditor
                .getState()
                .applyOps([{ type: 'item.update', itemId: item.id, patch: { props: { ...item.props, text } } } as Op], 'Edit text');
            }
          }}
        />
      )}
    </div>
  );
}

function BoxFrame({
  b,
  k,
  variant,
  children,
}: {
  b: ItemBounds;
  k: number;
  variant: 'hover' | 'selected';
  children?: React.ReactNode;
}) {
  return (
    <div
      className="absolute pointer-events-none"
      style={{
        left: b.cx * k,
        top: b.cy * k,
        width: b.w * k,
        height: b.h * k,
        transform: `translate(-50%, -50%) rotate(${b.rotation}deg)`,
        boxShadow: variant === 'selected' ? '0 0 0 1.5px #5FB7A1' : '0 0 0 1px rgba(95,183,161,0.45)',
      }}
    >
      {children}
    </div>
  );
}

const CORNERS = [
  { x: '0%', y: '0%' },
  { x: '100%', y: '0%' },
  { x: '100%', y: '100%' },
  { x: '0%', y: '100%' },
];

function Handles({
  b,
  onDown,
}: {
  b: ItemBounds;
  onDown: (e: React.PointerEvent, kind: 'scale' | 'rotate', corner?: number) => void;
}) {
  const cursorFor = (corner: number): string => {
    const base = corner % 2 === 0 ? 1 : 3; // tl/br = nwse, tr/bl = nesw
    return CURSORS[(((base + Math.round(b.rotation / 45)) % 4) + 4) % 4]!;
  };
  const ring = { boxShadow: '0 0 0 1px rgba(10,10,11,0.35)' };
  return (
    <>
      {CORNERS.map((c, i) => (
        <div
          key={i}
          className="absolute pointer-events-auto flex items-center justify-center"
          style={{ left: c.x, top: c.y, width: 20, height: 20, transform: 'translate(-50%, -50%)', cursor: cursorFor(i) }}
          onPointerDown={(e) => onDown(e, 'scale', i)}
        >
          <span className="block w-2.5 h-2.5 bg-white rounded-[2px] border border-accent" style={ring} />
        </div>
      ))}
      <div className="absolute w-px bg-accent pointer-events-none" style={{ left: '50%', top: -22, height: 22 }} />
      <div
        className="absolute pointer-events-auto flex items-center justify-center"
        style={{ left: '50%', top: -22, width: 22, height: 22, transform: 'translate(-50%, -50%)', cursor: 'grab' }}
        onPointerDown={(e) => onDown(e, 'rotate')}
      >
        <span className="block w-3 h-3 bg-white rounded-full border border-accent" style={ring} />
      </div>
    </>
  );
}

function TextEditor({
  b,
  item,
  k,
  onDone,
  onStart,
}: {
  b: ItemBounds;
  item: Item | undefined;
  k: number;
  onDone: (text: string | null) => void;
  onStart: () => void;
}) {
  const ref = useRef<HTMLTextAreaElement>(null);
  const done = useRef(false);
  const props = item?.props as
    | { text: string; style: { fontSize: number; fontFamily: string; fontWeight: number; color: string; lineHeight: number; align: string; uppercase: boolean } }
    | undefined;
  const [value, setValue] = useState(props?.text ?? '');
  useEffect(() => {
    onStart(); // hide the canvas copy of the text while the editable one is up
    ref.current?.focus();
    ref.current?.select();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  if (!props) return null;
  const s = props.style;
  const finish = (text: string | null) => {
    if (done.current) return;
    done.current = true;
    onDone(text);
  };
  return (
    <textarea
      ref={ref}
      value={value}
      spellCheck={false}
      rows={Math.max(1, value.split('\n').length)}
      onChange={(e) => setValue(e.target.value)}
      onBlur={() => finish(value)}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key === 'Escape') {
          e.preventDefault();
          finish(null);
        } else if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
          e.preventDefault();
          finish(value);
        }
      }}
      onPointerDown={(e) => e.stopPropagation()}
      onDoubleClick={(e) => e.stopPropagation()}
      className="absolute resize-none overflow-hidden outline-none bg-surface-950/70 rounded-[2px]"
      style={{
        left: b.cx * k,
        top: b.cy * k,
        width: Math.max(b.w * k, 120),
        minHeight: b.h * k,
        transform: `translate(-50%, -50%) rotate(${b.rotation}deg)`,
        font: `${s.fontWeight} ${s.fontSize * b.sx * k}px ${s.fontFamily}, Geist, system-ui, sans-serif`,
        lineHeight: s.lineHeight,
        color: s.color,
        textAlign: s.align === 'left' ? 'left' : s.align === 'right' ? 'right' : 'center',
        textTransform: s.uppercase ? 'uppercase' : 'none',
        boxShadow: '0 0 0 1.5px #5FB7A1',
        padding: 0,
        border: 0,
      }}
    />
  );
}
