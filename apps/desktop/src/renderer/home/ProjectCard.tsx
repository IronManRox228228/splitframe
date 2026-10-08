import { useEffect, useRef, useState } from 'react';
import type { ProjectSummary } from '../../preload/index.ts';
import { Icon } from '../ui/Icon.tsx';
import { Menu } from '../ui/Menu.tsx';
import { mediaUrl } from '../lib/url.ts';
import { aspectLabel, editedLabel, formatDuration } from './format.ts';

export function ProjectCard({
  project,
  renaming,
  onOpen,
  onStartRename,
  onCommitRename,
  onCancelRename,
  onDuplicate,
  onReveal,
  onDelete,
}: {
  project: ProjectSummary;
  renaming: boolean;
  onOpen(): void;
  onStartRename(): void;
  onCommitRename(name: string): void;
  onCancelRename(): void;
  onDuplicate(): void;
  onReveal(): void;
  onDelete(): void;
}) {
  const [thumbFailed, setThumbFailed] = useState(false);
  const showThumb = project.thumbPath && !thumbFailed;
  return (
    <div className="group relative glass rounded-[22px] p-2 pb-3 flex flex-col gap-2.5 min-w-0">
      <div className="relative">
        <button
          type="button"
          onClick={onOpen}
          aria-label={`Open ${project.name}`}
          className="block w-full relative aspect-[16/10] rounded-[15px] overflow-hidden bg-surface-850 shadow-[inset_0_0_0_1px_rgba(255,255,255,0.06)] hover:shadow-[inset_0_0_0_1px_rgba(255,255,255,0.18)] focus-visible:outline focus-visible:outline-2 focus-visible:outline-accent transition-shadow"
        >
          {showThumb ? (
            <img
              src={mediaUrl(project.thumbPath!)}
              alt=""
              draggable={false}
              onError={() => setThumbFailed(true)}
              className="absolute inset-0 w-full h-full object-cover"
            />
          ) : (
            <span className="absolute inset-0 flex items-center justify-center text-surface-600">
              <Icon name="media" size={28} strokeWidth={1.4} />
            </span>
          )}
          <span className="absolute left-2 bottom-2 px-2 py-0.5 rounded-full bg-surface-950/60 font-mono text-[11px] text-fg">
            {formatDuration(project.durationMs)}
          </span>
          <span className="absolute right-2 bottom-2 px-2 py-0.5 rounded-full bg-surface-950/60 text-[11px] text-fg-2">
            {aspectLabel(project.width, project.height)}
          </span>
        </button>
        <div className="absolute right-2 top-2 opacity-0 group-hover:opacity-100 focus-within:opacity-100 transition-opacity">
          <Menu
            trigger={({ toggle }) => (
              <button
                type="button"
                aria-label={`Actions for ${project.name}`}
                aria-haspopup="menu"
                onClick={toggle}
                className="w-[30px] h-[30px] rounded-full bg-surface-950/70 hover:bg-surface-950 flex items-center justify-center text-fg"
              >
                <Icon name="more" size={16} />
              </button>
            )}
            items={[
              { label: 'Rename', onSelect: onStartRename },
              { label: 'Duplicate', onSelect: onDuplicate },
              { label: 'Show in folder', onSelect: onReveal },
              { label: 'Delete', onSelect: onDelete, danger: true, separatorBefore: true },
            ]}
          />
        </div>
      </div>
      <div className="flex flex-col gap-0.5 px-1.5 min-w-0">
        {renaming ? (
          <RenameInput initial={project.name} onCommit={onCommitRename} onCancel={onCancelRename} />
        ) : (
          <button type="button" onClick={onOpen} className="text-left text-sm font-medium text-fg truncate" title={project.name}>
            {project.name}
          </button>
        )}
        <span className="text-xs text-fg-muted">{editedLabel(project.updatedAt)}</span>
      </div>
    </div>
  );
}

function RenameInput({ initial, onCommit, onCancel }: { initial: string; onCommit(name: string): void; onCancel(): void }) {
  const ref = useRef<HTMLInputElement>(null);
  const [value, setValue] = useState(initial);
  const done = useRef(false);
  useEffect(() => {
    ref.current?.focus();
    ref.current?.select();
  }, []);
  const finish = (commit: boolean) => {
    if (done.current) return;
    done.current = true;
    if (commit && value.trim() && value.trim() !== initial) onCommit(value.trim());
    else onCancel();
  };
  return (
    <input
      ref={ref}
      value={value}
      maxLength={120}
      aria-label="Project name"
      onChange={(e) => setValue(e.target.value)}
      onKeyDown={(e) => {
        if (e.key === 'Enter') finish(true);
        else if (e.key === 'Escape') finish(false);
      }}
      onBlur={() => finish(true)}
      className="input h-7 px-2 text-sm font-medium"
    />
  );
}
