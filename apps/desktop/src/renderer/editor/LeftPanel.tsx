import { useState } from 'react';
import { useEditor, type LeftPanelId, type JobInfo } from '../store.ts';
import { mediaUrl } from '../lib/url.ts';
import { shortcutLabel } from '../lib/platform-labels.ts';
import { Icon, type IconName } from '../ui/Icon.tsx';
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
      <nav aria-label="Panels" className="w-[72px] shrink-0 flex flex-col items-center gap-1 pt-2.5 border-r border-line">
        {RAIL.map((r) => {
          const active = panel === r.id;
          return (
            <button
              key={r.id}
              aria-label={r.label}
              aria-pressed={active}
              title={active ? `Hide ${r.label}` : r.label}
              onClick={() => setPanel(active ? null : r.id)}
              className={`w-[60px] h-14 rounded-xl flex flex-col items-center justify-center gap-1 transition-colors ${
                active ? 'bg-surface-800 text-fg' : 'text-fg-faint hover:text-fg hover:bg-surface-850'
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
        className={`w-[300px] shrink-0 border-r border-line bg-surface-900 flex-col min-h-0 ${panel ? 'flex' : 'hidden'}`}
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
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);
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
      <label className="flex items-center gap-2 h-9 px-3 rounded-[10px] bg-surface-850 border border-surface-800 text-fg-faint focus-within:border-accent/60">
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
                <div
                  key={asset.id}
                  className="flex flex-col gap-1.5 cursor-grab active:cursor-grabbing"
                  draggable
                  onDragStart={(e) => {
                    e.dataTransfer.setData('text/cutboard-asset', asset.id);
                    e.dataTransfer.effectAllowed = 'copy';
                  }}
                  onDoubleClick={() => void addAssetToTimeline(asset.id)}
                  title={`${asset.originalName} (drag to the timeline or double-click to add)`}
                >
                  <div className="aspect-[4/3] rounded-[10px] bg-surface-800 relative overflow-hidden">
                    {asset.thumbPath ? (
                      <img src={mediaUrl(asset.thumbPath)} alt="" className="w-full h-full object-cover" draggable={false} />
                    ) : (
                      <div className="w-full h-full flex items-center justify-center text-fg-faint">
                        <Icon name={asset.kind === 'audio' ? 'audio' : asset.kind === 'image' ? 'image' : 'media'} size={24} />
                      </div>
                    )}
                    {asset.kind !== 'image' && asset.durationMs > 0 && (
                      <span className="absolute right-1.5 bottom-1.5 px-1.5 py-0.5 rounded-md bg-surface-950/70 font-mono text-[11px] text-fg">
                        {formatDuration(asset.durationMs)}
                      </span>
                    )}
                    {activeJob && <ProgressBar value={activeJob.progress ?? 0} />}
                  </div>
                  <span className="text-xs text-fg-2 truncate">{asset.originalName}</span>
                  <AssetStatus asset={asset} job={activeJob} />
                </div>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}

function ProgressBar({ value }: { value: number }) {
  return (
    <span className="absolute left-1.5 right-1.5 top-1.5 h-[3px] rounded-sm bg-white/20 overflow-hidden" role="progressbar" aria-valuenow={Math.round(value * 100)}>
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
        <RelinkButton assetId={asset.id} label="Relink file" />
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
        <span className="w-1.5 h-1.5 rounded-full bg-amber-400" />
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
