import { useEffect } from 'react';
import { useEditor } from './store.ts';
import { ProjectListScreen } from './screens/ProjectListScreen.tsx';
import { EditorScreen } from './screens/EditorScreen.tsx';
import { Toaster } from './ui/Toaster.tsx';
import { DropImport } from './ui/DropImport.tsx';

export default function App() {
  const screen = useEditor((s) => s.screen);
  const bootstrap = useEditor((s) => s.bootstrap);
  const projectName = useEditor((s) => (s.screen === 'editor' ? s.doc?.project.name : undefined));

  useEffect(() => {
    void bootstrap();
  }, [bootstrap]);

  useEffect(() => {
    document.title = projectName ? `${projectName} — SplitFrame` : 'SplitFrame';
  }, [projectName]);

  return (
    <div className="h-full w-full flex flex-col bg-surface-950 text-fg">
      {screen === 'projects' ? <ProjectListScreen /> : <EditorScreen />}
      <DropImport />
      <Toaster />
    </div>
  );
}
