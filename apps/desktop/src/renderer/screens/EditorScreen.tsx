import { useEffect, useRef } from 'react';
import { useEditor } from '../store.ts';
import { TopBar } from '../editor/TopBar.tsx';
import { LeftPanel } from '../editor/LeftPanel.tsx';
import { PreviewPane } from '../editor/PreviewPane.tsx';
import { Inspector } from '../editor/Inspector.tsx';
import { Timeline } from '../editor/Timeline.tsx';
import { ExportDialog } from '../editor/ExportDialog.tsx';
import { handleKeyDown } from '../lib/shortcuts.ts';

export function EditorScreen() {
  const doc = useEditor((s) => s.doc);

  const containerRef = useRef<HTMLDivElement>(null);

  // keyboard shortcuts: resolution and actions live in lib/shortcuts.ts
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      handleKeyDown(e, () => useEditor.getState());
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, []);

  // J/K/L rate: v1 uses jump semantics for J (reverse unsupported in scrub model)
  if (!doc) return null;

  return (
    <div ref={containerRef} className="flex-1 flex flex-col min-h-0 bg-surface-950 p-3 gap-3">
      <TopBar />
      <div className="flex-1 flex min-h-0 gap-3">
        <LeftPanel />
        <PreviewPane />
        <Inspector />
      </div>
      <Timeline />
      <ExportDialog />
    </div>
  );
}

