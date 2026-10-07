import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';
import { Menu } from '../ui/Menu.tsx';

function gcd(a: number, b: number): number {
  return b === 0 ? a : gcd(b, a % b);
}

export function TopBar() {
  const doc = useEditor((s) => s.doc);
  const closeProject = useEditor((s) => s.closeProject);
  const applyOps = useEditor((s) => s.applyOps);
  const undo = useEditor((s) => s.undo);
  const redo = useEditor((s) => s.redo);
  const setExportDialog = useEditor((s) => s.setExportDialog);
  const revealProjectDir = useEditor((s) => s.revealProjectDir);
  const [name, setName] = useState(doc?.project.name ?? '');
  const [mcpOn, setMcpOn] = useState(false);
  const [history, setHistory] = useState({ canUndo: true, canRedo: true });

  useEffect(() => {
    void window.cutboard.mcpGetStatus().then((s) => setMcpOn(Boolean((s as { enabled?: boolean }).enabled)));
    return window.cutboard.onEvent((envelope) => {
      if (envelope.type === 'mcp:status') setMcpOn(Boolean((envelope.payload as { enabled?: boolean }).enabled));
    });
  }, []);

  useEffect(() => {
    setName(doc?.project.name ?? '');
  }, [doc?.project.name, doc?.project.id]);

  // the doc object changes on every edit, undo and redo, so it is the cue to re-ask the history
  useEffect(() => {
    let live = true;
    void window.cutboard
      .historyLabels()
      .then((h) => live && setHistory({ canUndo: h.canUndo, canRedo: h.canRedo }))
      .catch(() => undefined);
    return () => {
      live = false;
    };
  }, [doc]);

  if (!doc) return null;

  const { width, height, fps } = doc.project;
  const d = gcd(width, height) || 1;
  const subtitle = `Saved · ${width / d}:${height / d} · ${Math.min(width, height)}p ${fps}`;

  const commitName = () => {
    const trimmed = name.trim();
    if (trimmed && trimmed !== doc.project.name) {
      void applyOps([{ type: 'project.rename', name: trimmed }], 'Rename project');
    } else {
      setName(doc.project.name);
    }
  };

  const toggleMcp = () => {
    // the main process broadcasts mcp:status with the real state; only surface a failed start here
    void window.cutboard.mcpSetEnabled(!mcpOn).then((s) => {
      const error = (s as { error?: string } | undefined)?.error;
      if (error) useEditor.getState().showToast(`Couldn't start the MCP server: ${error}`, { kind: 'error' });
    });
  };

  return (
    <header className="h-14 shrink-0 flex items-center gap-3 px-4 border-b border-line select-none">
      <button className="icon-btn" aria-label="Back to projects" title="Back to projects" onClick={() => void closeProject()}>
        <Icon name="back" size={18} />
      </button>
      <div className="flex flex-col min-w-0">
        <input
          value={name}
          aria-label="Project name"
          onChange={(e) => setName(e.target.value)}
          onBlur={commitName}
          onKeyDown={(e) => {
            if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
            if (e.key === 'Escape') {
              setName(doc.project.name);
              (e.target as HTMLInputElement).blur();
            }
          }}
          className="bg-transparent border border-transparent hover:border-surface-700 focus:border-surface-600 rounded-md -ml-1.5 px-1.5 text-sm font-medium text-fg outline-none w-56 h-6"
        />
        <span className="text-[11px] text-fg-faint leading-4">{subtitle}</span>
      </div>

      <div className="flex-1" />
      <div className="flex gap-1">
        <button className="icon-btn" aria-label="Undo" title="Undo (Ctrl+Z)" disabled={!history.canUndo} onClick={() => void undo()}>
          <Icon name="undo" size={18} />
        </button>
        <button className="icon-btn" aria-label="Redo" title="Redo (Ctrl+Shift+Z)" disabled={!history.canRedo} onClick={() => void redo()}>
          <Icon name="redo" size={18} />
        </button>
      </div>
      <div className="flex-1" />

      <button
        className={`icon-btn ${mcpOn ? 'text-success' : ''}`}
        aria-label={mcpOn ? 'MCP server on' : 'MCP server off'}
        aria-pressed={mcpOn}
        title={
          mcpOn
            ? 'MCP server is on: external AI tools (Claude, Cursor...) can edit this project. Click to turn off.'
            : 'MCP server is off. Click to let external AI tools connect and edit this project.'
        }
        onClick={toggleMcp}
      >
        <Icon name="plug" size={18} />
      </button>
      <Menu
        trigger={({ toggle, open }) => (
          <button className="icon-btn" aria-label="More" aria-haspopup="menu" aria-expanded={open} title="More" onClick={toggle}>
            <Icon name="more" size={18} />
          </button>
        )}
        items={[{ label: 'Show project folder', onSelect: () => void revealProjectDir() }]}
      />
      <button className="btn-primary" onClick={() => setExportDialog(true)}>
        Export
      </button>
    </header>
  );
}
