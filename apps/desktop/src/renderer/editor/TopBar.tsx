import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';

export function TopBar() {
  const doc = useEditor((s) => s.doc);
  const closeProject = useEditor((s) => s.closeProject);
  const applyOps = useEditor((s) => s.applyOps);
  const setExportDialog = useEditor((s) => s.setExportDialog);
  const revealProjectDir = useEditor((s) => s.revealProjectDir);
  const [name, setName] = useState(doc?.project.name ?? '');
  const [mcpOn, setMcpOn] = useState(false);

  useEffect(() => {
    void window.cutboard.mcpGetStatus().then((s) => setMcpOn(Boolean((s as { enabled?: boolean }).enabled)));
    return window.cutboard.onEvent((envelope) => {
      if (envelope.type === 'mcp:status') setMcpOn(Boolean((envelope.payload as { enabled?: boolean }).enabled));
    });
  }, []);

  useEffect(() => {
    setName(doc?.project.name ?? '');
  }, [doc?.project.name, doc?.project.id]);

  if (!doc) return null;

  const commitName = () => {
    const trimmed = name.trim();
    if (trimmed && trimmed !== doc.project.name) {
      void applyOps([{ type: 'project.rename', name: trimmed }], 'Rename project');
    } else {
      setName(doc.project.name);
    }
  };

  return (
    <div className="h-12 flex items-center gap-3 px-4 border-b border-line bg-surface-950/95 select-none">
      <button
        className="btn-ghost w-7 px-0 justify-center"
        title="Back to projects"
        onClick={() => void closeProject()}
      >
        ←
      </button>
      <input
        value={name}
        onChange={(e) => setName(e.target.value)}
        onBlur={commitName}
        onKeyDown={(e) => {
          if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
        }}
        className="bg-transparent border border-transparent hover:border-line focus:border-accent-dim rounded px-2 py-1 text-sm font-medium text-fg outline-none w-56"
      />
      <span className="chip bg-surface-700 text-fg-muted">
        {doc.project.width}×{doc.project.height} · {doc.project.fps} fps
      </span>
      <div className="flex-1" />
      <span className={`flex items-center gap-1.5 text-[11px] ${mcpOn ? 'text-emerald-400' : 'text-fg-faint'}`}>
        <span className={`w-1.5 h-1.5 rounded-full ${mcpOn ? 'bg-emerald-400' : 'bg-surface-800'}`} />
        {mcpOn ? 'MCP on' : 'MCP off'}
      </span>
      <button className="btn-ghost" title="Reveal project folder" onClick={() => void revealProjectDir()}>
        Reveal
      </button>
      <button className="btn-primary" onClick={() => setExportDialog(true)}>
        Export
      </button>
    </div>
  );
}
