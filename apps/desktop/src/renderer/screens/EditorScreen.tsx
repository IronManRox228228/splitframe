import { useEffect, useRef } from 'react';
import { useEditor } from '../store.ts';
import { TopBar } from '../editor/TopBar.tsx';
import { LeftPanel } from '../editor/LeftPanel.tsx';
import { PreviewPane } from '../editor/PreviewPane.tsx';
import { Timeline } from '../editor/Timeline.tsx';
import { ExportDialog } from '../editor/ExportDialog.tsx';
import { shortcutFor } from '../lib/shortcuts.ts';

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
      const action = shortcutFor(
        {
          key: e.key,
          code: e.code,
          metaKey: e.metaKey,
          ctrlKey: e.ctrlKey,
          shiftKey: e.shiftKey,
          targetTag: target.tagName,
          isContentEditable: target.isContentEditable,
        },
        { playing },
      );
      if (!action) return;
      e.preventDefault();
      switch (action) {
        case 'togglePlay': togglePlay(); break;
        case 'play': if (!playing) togglePlay(); break;
        case 'pause': if (playing) togglePlay(); break;
        case 'undo': void undo(); break;
        case 'redo': void redo(); break;
        case 'clone': void cloneSelection(); break;
        case 'import': void importMedia(); break;
        case 'delete': void deleteSelection(); break;
        case 'split': void splitAtPlayhead(); break;
        case 'stepBack': stepFrames(e.shiftKey ? -30 : -1); break;
        case 'stepForward': stepFrames(e.shiftKey ? 30 : 1); break;
        case 'jumpBack': stepFrames(-30); break;
        case 'zoomIn': setPxPerFrame(pxPerFrame * 1.25); break;
        case 'zoomOut': setPxPerFrame(pxPerFrame / 1.25); break;
        case 'zoomFit': {
          const current = useEditor.getState().doc;
          if (current) zoomFit(Math.max(1, docDuration(current)));
          break;
        }
        case 'deselect': select(null); break;
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
