import { useEffect } from 'react';
import { useEditor } from './store.ts';
import { ProjectListScreen } from './screens/ProjectListScreen.tsx';
import { EditorScreen } from './screens/EditorScreen.tsx';

export default function App() {
  const screen = useEditor((s) => s.screen);
  const bootstrap = useEditor((s) => s.bootstrap);
  const toast = useEditor((s) => s.toast);

  useEffect(() => {
    void bootstrap();
  }, [bootstrap]);

  return (
    <div className="h-full w-full flex flex-col">
      {screen === 'projects' ? <ProjectListScreen /> : <EditorScreen />}
      {toast && (
        <div className="fixed bottom-5 left-1/2 -translate-x-1/2 z-50 bg-surface-800 border border-line text-neutral-100 text-xs px-4 py-2 rounded-lg shadow-lg max-w-[70ch]">
          {toast}
        </div>
      )}
    </div>
  );
}
