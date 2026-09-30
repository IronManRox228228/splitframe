import { useEffect, useRef } from 'react';
import { useEditor } from '../store.ts';
import { TopBar } from '../editor/TopBar.tsx';
import { LeftPanel } from '../editor/LeftPanel.tsx';
import { PreviewPane } from '../editor/PreviewPane.tsx';
import { Timeline } from '../editor/Timeline.tsx';
import { ExportDialog } from '../editor/ExportDialog.tsx';

export function EditorScreen() {
  const doc = useEditor((s) => s.doc);
  const playing = useEditor((s) => s.playing);
  const togglePlay = useEditor((s) => s.togglePlay);
  const stepFrames = useEditor((s) => s.stepFrames);
  const undo = useEditor((s) => s.undo);
  const redo = useEditor((s) => s.redo);
  const deleteSelection = useEditor((s) => s.deleteSelection);
  const splitAtPlayhead = useEditor((s) => s.splitAtPlayhead);
  const cloneSelection = useEditor((s) => s.cloneSelection);
  const pxPerFrame = useEditor((s) => s.pxPerFrame);
  const setPxPerFrame = useEditor((s) => s.setPxPerFrame);
  const zoomFit = useEditor((s) => s.zoomFit);
  const importMedia = useEditor((s) => s.importMedia);
  const select = useEditor((s) => s.select);

  const containerRef = useRef<HTMLDivElement>(null);

  // keyboard shortcuts (addendum §5.3): Space, J/K/L, S, arrows, undo/redo
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable) return;
      const meta = e.metaKey || e.ctrlKey;
      if (e.code === 'Space') {
        e.preventDefault();
        togglePlay();
      } else if (meta && e.key.toLowerCase() === 'z') {
        e.preventDefault();
        if (e.shiftKey) void redo();
        else void undo();
      } else if (meta && e.key.toLowerCase() === 'd') {
        e.preventDefault();
        void cloneSelection();
      } else if (meta && e.key.toLowerCase() === 'i') {
        e.preventDefault();
        void importMedia();
      } else if (e.key === 'Backspace' || e.key === 'Delete') {
        e.preventDefault();
        void deleteSelection();
      } else if (e.key.toLowerCase() === 's' && !meta) {
        e.preventDefault();
        void splitAtPlayhead();
      } else if (e.key === 'ArrowLeft') {
        e.preventDefault();
        stepFrames(e.shiftKey ? -30 : -1);
      } else if (e.key === 'ArrowRight') {
        e.preventDefault();
        stepFrames(e.shiftKey ? 30 : 1);
      } else if (e.key === '+' || e.key === '=') {
        setPxPerFrame(pxPerFrame * 1.25);
      } else if (e.key === '-') {
        setPxPerFrame(pxPerFrame / 1.25);
      } else if (e.key.toLowerCase() === 'j') {
        stepFrames(-30);
      } else if (e.key.toLowerCase() === 'k') {
        if (playing) togglePlay();
      } else if (e.key.toLowerCase() === 'l') {
        if (!playing) togglePlay();
      } else if (e.key.toLowerCase() === 'z' && e.shiftKey) {
        const doc = useEditor.getState().doc;
        if (doc) zoomFit(Math.max(1, docDuration(doc)));
      } else if (e.key === 'Escape') {
        select(null);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [playing, pxPerFrame, togglePlay, stepFrames, undo, redo, deleteSelection, splitAtPlayhead, cloneSelection, setPxPerFrame, zoomFit, importMedia, select]);

  // J/K/L rate: v1 uses jump semantics for J (reverse unsupported in scrub model)
  if (!doc) return null;

  return (
    <div ref={containerRef} className="flex-1 flex flex-col min-h-0">
      <TopBar />
      <div className="flex-1 flex min-h-0">
        <LeftPanel />
        <div className="flex-1 flex flex-col min-w-0 min-h-0">
          <PreviewPane />
          <Timeline />
        </div>
      </div>
      <ExportDialog />
    </div>
  );
}

function docDuration(doc: { items: { startFrame: number; durationFrames: number }[] }): number {
  return doc.items.reduce((m, i) => Math.max(m, i.startFrame + i.durationFrames), 0);
}
