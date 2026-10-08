import { useEffect, useRef } from 'react';
import { formatTimecode } from '@cutboard/schema';
import { useEditor } from '../store.ts';

/**
 * The playhead moves every animation frame during playback. Anything that follows it
 * subscribes here directly instead of through a parent's render, so a playing timeline
 * doesn't re-render every clip, ruler tick and panel sixty times a second.
 */
export function usePlayheadEffect(fn: (playhead: number) => void, deps: unknown[]): void {
  const fnRef = useRef(fn);
  fnRef.current = fn;
  useEffect(() => {
    fnRef.current(useEditor.getState().playhead);
    return useEditor.subscribe((s, prev) => {
      if (s.playhead !== prev.playhead) fnRef.current(s.playhead);
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
}

/** The playhead as a timecode; re-renders only when the whole frame changes. */
export function PlayheadTimecode({ fps }: { fps: number }) {
  const frame = useEditor((s) => Math.round(s.playhead));
  return <>{formatTimecode(frame, fps)}</>;
}

/** An absolutely positioned marker moved with a transform, without React re-rendering it. */
export function PlayheadMarker({
  pxPerFrame,
  offset = 0,
  className,
  children,
}: {
  pxPerFrame: number;
  offset?: number;
  className: string;
  children?: React.ReactNode;
}) {
  const ref = useRef<HTMLDivElement>(null);
  usePlayheadEffect(
    (playhead) => {
      if (ref.current) ref.current.style.transform = `translateX(${offset + playhead * pxPerFrame}px)`;
    },
    [pxPerFrame, offset],
  );
  return (
    <div ref={ref} className={className} style={{ left: 0 }}>
      {children}
    </div>
  );
}
