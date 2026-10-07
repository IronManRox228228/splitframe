import { useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from './Icon.tsx';

/** External file drags only: the app's own asset drags to the timeline carry no 'Files' type. */
const isFileDrag = (e: DragEvent): boolean => {
  const types = Array.from(e.dataTransfer?.types ?? []);
  return types.includes('Files') && !types.includes('text/cutboard-asset');
};

/**
 * Window-wide "drop to import". Files dropped on the editor are imported into the open
 * project; on the Home screen a 16:9 project is created first. Also stops Electron from
 * navigating to a dropped file. Paths come from webUtils (preload); main re-validates them.
 */
export function DropImport() {
  const [active, setActive] = useState(false);
  const depth = useRef(0);

  useEffect(() => {
    const onEnter = (e: DragEvent) => {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      depth.current++;
      setActive(true);
    };
    const onOver = (e: DragEvent) => {
      if (!isFileDrag(e)) return;
      e.preventDefault(); // required to allow the drop, and keeps the browser from opening the file
      if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
    };
    const onLeave = (e: DragEvent) => {
      if (!isFileDrag(e)) return;
      depth.current = Math.max(0, depth.current - 1);
      if (depth.current === 0) setActive(false);
    };
    const onDrop = (e: DragEvent) => {
      if (!isFileDrag(e)) return;
      e.preventDefault();
      depth.current = 0;
      setActive(false);
      const files = Array.from(e.dataTransfer?.files ?? []);
      const paths = window.cutboard.getPathsForFiles(files);
      if (paths.length === 0) return;
      const s = useEditor.getState();
      void (async () => {
        try {
          if (s.screen !== 'editor') await s.createProject(undefined, { width: 1920, height: 1080 });
          await useEditor.getState().importPaths(paths.slice(0, 200));
        } catch (err) {
          s.showToast(err instanceof Error ? err.message : String(err), { kind: 'error' });
        }
      })();
    };
    window.addEventListener('dragenter', onEnter);
    window.addEventListener('dragover', onOver);
    window.addEventListener('dragleave', onLeave);
    window.addEventListener('drop', onDrop);
    return () => {
      window.removeEventListener('dragenter', onEnter);
      window.removeEventListener('dragover', onOver);
      window.removeEventListener('dragleave', onLeave);
      window.removeEventListener('drop', onDrop);
    };
  }, []);

  if (!active) return null;
  return (
    <div className="fixed inset-0 z-50 pointer-events-none bg-surface-950/80 flex items-center justify-center p-6" role="presentation">
      <div className="w-full h-full rounded-2xl border-2 border-dashed border-accent flex flex-col items-center justify-center gap-3 text-fg">
        <Icon name="upload" size={36} strokeWidth={1.6} />
        <p className="text-lg font-semibold">Drop to import</p>
        <p className="text-sm text-fg-muted">Video, audio and images</p>
      </div>
    </div>
  );
}
