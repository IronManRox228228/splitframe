import { useEffect, useState } from 'react';
import { useEditor, type LeftPanelId, type JobInfo } from '../store.ts';
import { mediaUrl } from '../lib/url.ts';
import { shortcutLabel } from '../lib/platform-labels.ts';
import { Icon, type IconName } from '../ui/Icon.tsx';
import { Dialog } from '../ui/Dialog.tsx';
import { ChatTab } from './ChatTab.tsx';
import { AudioPanel, CaptionsPanel, TextPanel } from './panels.tsx';

const RAIL: { id: LeftPanelId; label: string; icon: IconName }[] = [
  { id: 'media', label: 'Media', icon: 'media' },
  { id: 'text', label: 'Text', icon: 'text' },
  { id: 'audio', label: 'Audio', icon: 'audio' },
  { id: 'captions', label: 'Captions', icon: 'captions' },
  { id: 'assistant', label: 'Assistant', icon: 'sparkles' },
];

/** Icon rail plus the one open panel. Clicking the active rail item collapses the panel. */
export function LeftPanel() {
  const panel = useEditor((s) => s.leftPanel);
  const setPanel = useEditor((s) => s.setLeftPanel);
  return (
    <>
      <nav aria-label="Panels" className="relative glass w-[76px] shrink-0 rounded-[22px] flex flex-col items-center gap-1 py-2.5">
        {RAIL.map((r) => {
          const active = panel === r.id;
          return (
            <button
              key={r.id}
              aria-label={r.label}
              aria-pressed={active}
              title={active ? `Hide ${r.label}` : r.label}
              onClick={() => setPanel(active ? null : r.id)}
              className={`w-[64px] h-[54px] rounded-2xl flex flex-col items-center justify-center gap-1 transition-colors ${
                active ? 'bg-fg/10 text-fg' : 'text-fg-muted hover:text-fg hover:bg-white/[0.06]'
              }`}
            >
              <Icon name={r.icon} size={20} />
              <span className="text-[11px]">{r.label}</span>
            </button>
          );
        })}
      </nav>
      <aside
        aria-label="Panel"
        className={`relative glass w-[292px] shrink-0 rounded-[22px] flex-col min-h-0 overflow-hidden ${panel ? 'flex' : 'hidden'}`}
      >
        {panel === 'media' && <MediaPanel />}
        {panel === 'text' && <TextPanel />}
        {panel === 'audio' && <AudioPanel />}
        {panel === 'captions' && <CaptionsPanel />}
        {/* stays mounted (hidden) so closing the panel doesn't lose the conversation or orphan a running reply */}
        <div className={panel === 'assistant' ? 'flex-1 flex flex-col min-h-0' : 'hidden'}>
          <ChatTab />
        </div>
      </aside>
    </>
  );
}

type Filter = 'all' | 'video' | 'audio' | 'image';
const FILTERS: { id: Filter; label: string }[] = [
  { id: 'all', label: 'All' },
  { id: 'video', label: 'Video' },
  { id: 'audio', label: 'Audio' },
  { id: 'image', label: 'Images' },
];

function MediaPanel() {
  const assets = useEditor((s) => s.assets);
  const importMedia = useEditor((s) => s.importMedia);
  const platform = useEditor((s) => s.appInfo?.platform);
  const jobs = useEditor((s) => s.jobs);
  const doc = useEditor((s) => s.doc);
  const [pendingRemove, setPendingRemove] = useState<{ id: string; name: string; uses: number } | null>(null);
  const [query, setQuery] = useState('');
  const [filter, setFilter] = useState<Filter>('all');

  const q = query.trim().toLowerCase();
  const filtered = assets.filter((a) => (filter === 'all' || a.kind === filter) && a.originalName.toLowerCase().includes(q));

  return (
    <div className="flex-1 flex flex-col min-h-0 gap-3.5 p-4">
      <div className="flex items-center justify-between">
        <h2 className="heading">Media</h2>
        <button className="btn-secondary btn-sm" onClick={() => void importMedia()} title={`Import media (${shortcutLabel('I', platform)})`}>
          <Icon name="upload" size={14} />
          Upload
        </button>
      </div>
      <label className="flex items-center gap-2 h-[38px] px-3.5 rounded-full bg-[rgba(8,10,9,0.4)] border border-line text-fg-muted focus-within:border-accent/60">
        <Icon name="search" size={15} />
        <input
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search by file name"
          aria-label="Search media"
          className="bg-transparent outline-none text-sm text-fg placeholder:text-fg-faint w-full"
        />
      </label>
      <div className="flex gap-1.5" role="group" aria-label="Filter media">
        {FILTERS.map((f) => (
          <button key={f.id} className={filter === f.id ? 'pill-active' : 'pill'} aria-pressed={filter === f.id} onClick={() => setFilter(f.id)}>
            {f.label}
          </button>
        ))}
      </div>

      <div className="flex-1 min-h-0 overflow-y-auto -mr-2 pr-2">
        {assets.length === 0 ? (
          <div className="flex flex-col items-center text-center gap-3 pt-10 text-fg-muted">
            <Icon name="media" size={28} />
            <p className="text-fg text-sm font-medium">No media yet</p>
            <p className="text-xs">Import video, audio or images to start editing.</p>
            <button className="btn-primary" onClick={() => void importMedia()}>
              <Icon name="upload" size={16} />
              Import media
            </button>
          </div>
        ) : filtered.length === 0 ? (
          <div className="text-center pt-10 text-xs text-fg-muted">
            {q ? `No matches for '${query.trim()}'` : `No ${filter === 'image' ? 'images' : filter} files yet`}
            <div className="mt-3">
              <button
                className="btn-outline btn-sm mx-auto"
                onClick={() => {
                  setQuery('');
                  setFilter('all');
                }}
              >
                Clear search
              </button>
            </div>
          </div>
        ) : (
          <div className="grid grid-cols-2 gap-2.5 content-start">
            {filtered.map((asset) => {
              const activeJob = Object.values(jobs).find(
                (j) => j.type === 'ingest-asset' && j.payload?.assetId === asset.id && (j.status === 'running' || j.status === 'pending'),
              );
              return (
                <MediaCard
                  key={asset.id}
                  asset={asset}
                  job={activeJob}
                  onRemove={() => {
                    const uses = doc?.items.filter((i) => i.assetId === asset.id).length ?? 0;
                    if (uses === 0) void useEditor.getState().removeAssetFromProject(asset.id, false);
                    else setPendingRemove({ id: asset.id, name: asset.originalName, uses });
                  }}
                />
              );
            })}
          </div>
        )}
      </div>
      {pendingRemove && (
        <Dialog title="Remove from project?" width={440} onClose={() => setPendingRemove(null)}>
          <div className="px-6 pt-3 pb-6 flex flex-col gap-5">
            <p className="text-sm text-fg-2">
              &ldquo;{pendingRemove.name}&rdquo; is used in {pendingRemove.uses} clip{pendingRemove.uses === 1 ? '' : 's'} on the timeline. Remove {pendingRemove.uses === 1 ? 'it' : 'them'} too? Your original file is not
              deleted. Removing the media itself cannot be undone.
            </p>
            <div className="flex justify-end gap-2">
              <button className="btn-outline" onClick={() => setPendingRemove(null)}>
                Cancel
              </button>
              <button
                className="btn-danger"
                onClick={() => {
                  const id = pendingRemove.id;
                  setPendingRemove(null);
                  void useEditor.getState().removeAssetFromProject(id, true);
                }}
              >
                Remove clips and media
              </button>
            </div>
          </div>
        </Dialog>
      )}
    </div>
  );
}

interface MenuState {
  x: number;
  y: number;
}

interface CardAction {
  label: string;
  run(): void;
  danger?: boolean;
  disabled?: boolean;
  separatorBefore?: boolean;
}

function MediaCard({ asset, job, onRemove }: { asset: AssetT; job: JobInfo | undefined; onRemove(): void }) {
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const missing = asset.status === 'missing';
  const busy = Boolean(job) || asset.status === 'processing' || asset.status === 'importing';

  const items: CardAction[] = [
    { label: 'Add to timeline', run: () => void addAssetToTimeline(asset.id), disabled: missing || asset.status === 'failed' },
    {
      label: 'Show in folder',
      run: () =>
        void window.cutboard.revealAsset(asset.id).then((ok) => {
          if (!ok) useEditor.getState().showToast('The original file could not be found.', { kind: 'error' });
        }),
    },
    { label: 'Re-analyze', run: () => void useEditor.getState().reanalyzeAsset(asset.id), disabled: busy || missing },
    { label: 'Remove from project', run: onRemove, danger: true, separatorBefore: true },
  ];

  return (
    <div
      className="group relative flex flex-col gap-1.5 cursor-grab active:cursor-grabbing rounded-[14px] outline-none focus-visible:ring-2 focus-visible:ring-accent"
      tabIndex={0}
      role="group"
      aria-label={asset.originalName}
      draggable
      onDragStart={(e) => {
        e.dataTransfer.setData('text/cutboard-asset', asset.id);
        e.dataTransfer.effectAllowed = 'copy';
      }}
      onDoubleClick={() => void addAssetToTimeline(asset.id)}
      onContextMenu={(e) => {
        e.preventDefault();
        setMenu({ x: e.clientX, y: e.clientY });
      }}
      onKeyDown={(e) => {
        if (e.target !== e.currentTarget) return;
        if (e.key === 'Enter') {
          e.preventDefault();
          e.stopPropagation();
          void addAssetToTimeline(asset.id);
        } else if (e.key === 'Delete' || e.key === 'Backspace') {
          e.preventDefault();
          e.stopPropagation();
          onRemove();
        } else if (e.key === 'ContextMenu' || (e.key === 'F10' && e.shiftKey)) {
          e.preventDefault();
          const r = e.currentTarget.getBoundingClientRect();
          setMenu({ x: r.left + 12, y: r.top + 12 });
        }
      }}
      title={missing ? `Original not found: ${asset.path}` : `${asset.originalName} (drag to the timeline or double-click to add)`}
    >
      <div className="aspect-[4/3] rounded-[14px] bg-surface-800 shadow-[inset_0_0_0_1px_rgba(255,255,255,0.08)] relative overflow-hidden">
        {asset.thumbPath ? (
          <img src={mediaUrl(asset.thumbPath)} alt="" className="w-full h-full object-cover" draggable={false} />
        ) : (
          <div className="w-full h-full flex items-center justify-center text-fg-faint">
            <Icon name={asset.kind === 'audio' ? 'audio' : asset.kind === 'image' ? 'image' : 'media'} size={24} />
          </div>
        )}
        {asset.kind !== 'image' && asset.durationMs > 0 && (
          <span className="absolute right-1.5 bottom-1.5 px-2 py-0.5 rounded-full bg-surface-950/60 font-mono text-[11px] text-fg">
            {formatDuration(asset.durationMs)}
          </span>
        )}
        {job && <ProgressBar value={job.progress ?? 0} />}
        <button
          className="absolute left-1.5 top-1.5 w-6 h-6 rounded-full bg-surface-950/70 text-fg flex items-center justify-center opacity-0 group-hover:opacity-100 group-focus-within:opacity-100 focus-visible:opacity-100 hover:bg-surface-950"
          aria-label={`Actions for ${asset.originalName}`}
          aria-haspopup="menu"
          onClick={(e) => {
            e.stopPropagation();
            const r = e.currentTarget.getBoundingClientRect();
            setMenu({ x: r.left, y: r.bottom + 4 });
          }}
          onDoubleClick={(e) => e.stopPropagation()}
        >
          <Icon name="more" size={14} />
        </button>
      </div>
      <span className="text-xs text-fg-2 truncate">{asset.originalName}</span>
      <AssetStatus asset={asset} job={job} />
      {menu && <CardMenu at={menu} items={items} onClose={() => setMenu(null)} />}
    </div>
  );
}

/** Fixed-position action menu used by both right-click and the hover button. */
function CardMenu({ at, items, onClose }: { at: MenuState; items: CardAction[]; onClose(): void }) {
  useEffect(() => {
    const onDown = (e: MouseEvent) => {
      if (!(e.target as HTMLElement).closest('[data-card-menu]')) onClose();
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        onClose();
      }
    };
    window.addEventListener('mousedown', onDown);
    window.addEventListener('keydown', onKey, true);
    window.addEventListener('blur', onClose);
    document.querySelector<HTMLElement>('[data-card-menu] [role="menuitem"]:not([disabled])')?.focus();
    return () => {
      window.removeEventListener('mousedown', onDown);
      window.removeEventListener('keydown', onKey, true);
      window.removeEventListener('blur', onClose);
    };
  }, [onClose]);

  const left = Math.min(at.x, window.innerWidth - 200);
  const top = Math.min(at.y, window.innerHeight - 170);
  return (
    <div
      data-card-menu
      role="menu"
      style={{ left, top }}
      className="fixed z-30 min-w-[184px] p-1.5 glass-strong rounded-2xl flex flex-col cursor-default"
      onClick={(e) => e.stopPropagation()}
      onDoubleClick={(e) => e.stopPropagation()}
      onKeyDown={(e) => {
        e.stopPropagation();
        if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
        e.preventDefault();
        const els = [...e.currentTarget.querySelectorAll<HTMLElement>('[role="menuitem"]:not([disabled])')];
        const i = els.indexOf(document.activeElement as HTMLElement);
        els[(i + (e.key === 'ArrowDown' ? 1 : -1) + els.length) % els.length]?.focus();
      }}
    >
      {items.map((it) => (
        <div key={it.label} className="contents">
          {it.separatorBefore && <div className="h-px bg-line my-1" />}
          <button
            role="menuitem"
            disabled={it.disabled}
            onClick={() => {
              onClose();
              it.run();
            }}
            className={`h-8 px-2.5 rounded-xl text-left text-sm disabled:opacity-40 hover:bg-white/[0.08] focus:bg-white/[0.08] outline-none focus-visible:ring-1 focus-visible:ring-accent ${it.danger ? 'text-danger' : 'text-fg-2 hover:text-fg'}`}
          >
            {it.label}
          </button>
        </div>
      ))}
    </div>
  );
}

function ProgressBar({ value }: { value: number }) {
  return (
    <span className="absolute left-1.5 right-1.5 top-1.5 h-[3px] rounded-full bg-white/20 overflow-hidden" role="progressbar" aria-valuenow={Math.round(value * 100)}>
      <span className="block h-full bg-accent" style={{ width: `${Math.max(4, Math.round(value * 100))}%` }} />
    </span>
  );
}

function stageWord(stage: string | null | undefined): string {
  const s = (stage ?? '').toLowerCase();
  if (s.includes('transcri') || s.includes('asr') || s.includes('speech')) return 'Transcribing';
  if (s.includes('scene')) return 'Finding scenes';
  if (s.includes('embed') || s.includes('index')) return 'Indexing';
  if (s.includes('thumb') || s.includes('proxy') || s.includes('wave')) return 'Preparing preview';
  if (s.includes('probe') || s.includes('import')) return 'Importing';
  return 'Analyzing';
}

type AssetT = ReturnType<typeof useEditor.getState>['assets'][number];

function AssetStatus({ asset, job }: { asset: AssetT; job?: JobInfo }) {
  if (asset.status === 'missing') {
    return (
      <div className="flex flex-col gap-1">
        <span className="text-[11px] text-danger leading-snug">File not found at its original location</span>
        <span className="text-[11px] text-fg-faint leading-snug break-all line-clamp-2" title={asset.path}>
          {asset.path}
        </span>
        <RelinkButton assetId={asset.id} />
      </div>
    );
  }
  if (asset.status === 'failed') {
    return (
      <div className="flex flex-col gap-1">
        <span className="text-[11px] text-danger leading-snug">Analysis failed</span>
        {asset.error && (
          <span className="text-[11px] text-fg-muted leading-snug line-clamp-2" title={asset.error}>
            {asset.error}
          </span>
        )}
        <div className="flex gap-1.5">
          <button className="btn-outline btn-sm" onClick={() => void useEditor.getState().reanalyzeAsset(asset.id)}>
            <Icon name="retry" size={13} />
            Retry
          </button>
          <RelinkButton assetId={asset.id} label="Relink file" />
        </div>
      </div>
    );
  }
  if (job || asset.status === 'importing' || asset.status === 'processing') {
    const pct = job ? ` ${Math.round((job.progress ?? 0) * 100)}%` : '';
    return (
      <span className="text-[11px] text-fg-muted flex items-center gap-1.5">
        <span className="w-1.5 h-1.5 rounded-full bg-accent animate-pulse" />
        {job ? stageWord(job.stage ?? asset.stage) : asset.status === 'importing' ? 'Importing' : 'Analyzing'}
        {pct}
      </span>
    );
  }
  if (asset.status === 'analyzed' && asset.error?.startsWith('Transcription')) {
    return (
      <span className="text-[11px] text-fg-muted flex items-center gap-1.5" title={asset.error}>
        <span className="w-1.5 h-1.5 rounded-full bg-[#E7D9B8]" />
        No transcript
      </span>
    );
  }
  return (
    <span className="text-[11px] text-fg-muted flex items-center gap-1.5">
      <span className="w-1.5 h-1.5 rounded-full bg-success" />
      Ready
    </span>
  );
}

function RelinkButton({ assetId, label = 'Relink' }: { assetId: string; label?: string }) {
  const relink = async () => {
    const newPath = await window.cutboard.pickFilePath();
    if (!newPath) return;
    try {
      await window.cutboard.relinkAsset(assetId, newPath);
      useEditor.getState().showToast('Media relinked.', { kind: 'success' });
    } catch (err) {
      useEditor.getState().showToast(`Relink failed: ${err instanceof Error ? err.message : String(err)}`, { kind: 'error' });
    }
  };
  return (
    <button className="btn-outline btn-sm self-start" onClick={() => void relink()}>
      {label}
    </button>
  );
}

function formatDuration(ms: number): string {
  const s = Math.round(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
