import { useState } from 'react';
import { useEditor } from '../store.ts';
import { mediaUrl } from '../lib/url.ts';
import { shortcutLabel } from '../lib/platform-labels.ts';
import { ChatTab } from './ChatTab.tsx';

export function LeftPanel() {
  const [tab, setTab] = useState<'footage' | 'references' | 'chat'>('footage');
  return (
    <div className="w-[300px] shrink-0 border-r border-line bg-surface-950 flex flex-col min-h-0">
      <div className="flex border-b border-line">
        {(['footage', 'references', 'chat'] as const).map((t) => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={`flex-1 h-9 text-xs font-medium capitalize transition-colors ${
              tab === t ? 'text-accent border-b-2 border-accent -mb-px' : 'text-neutral-500 hover:text-neutral-300'
            }`}
          >
            {t === 'chat' ? 'AI chat' : t}
          </button>
        ))}
      </div>
      {tab === 'footage' && <FootageTab />}
      {tab === 'references' && (
        <div className="flex-1 flex items-center justify-center text-xs text-neutral-600 p-6 text-center">
          Reference style matching arrives with the References milestone. One reference video
          per project, analyzed for cut rhythm, caption style, and grade.
        </div>
      )}
      {/* stays mounted (hidden) so switching tabs doesn't lose the conversation or orphan a running reply */}
      <div className={tab === 'chat' ? 'flex-1 flex flex-col min-h-0' : 'hidden'}>
        <ChatTab />
      </div>
    </div>
  );
}

function FootageTab() {
  const assets = useEditor((s) => s.assets);
  const importMedia = useEditor((s) => s.importMedia);
  const platform = useEditor((s) => s.appInfo?.platform);
  const jobs = useEditor((s) => s.jobs);
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);
  const [query, setQuery] = useState('');

  const filtered = assets.filter((a) => a.originalName.toLowerCase().includes(query.toLowerCase()));

  return (
    <div className="flex-1 flex flex-col min-h-0">
      <div className="p-3 flex gap-2">
        <input
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          placeholder="Search footage…"
          className="flex-1 bg-surface-800 border border-line rounded px-2.5 h-7 text-xs outline-none focus:border-accent-dim"
        />
        <button className="btn-primary" onClick={() => void importMedia()} title={`Import media (${shortcutLabel('I', platform)})`}>
          Import
        </button>
      </div>
      <div className="flex-1 overflow-y-auto px-3 pb-3 grid grid-cols-2 gap-2 content-start">
        {filtered.map((asset) => {
          const activeJob = Object.values(jobs).find(
            (j) => j.type === 'ingest-asset' && j.payload?.assetId === asset.id && (j.status === 'running' || j.status === 'pending'),
          );
          const status =
            asset.status === 'missing'
              ? 'missing'
              : activeJob
                ? `${activeJob.stage ?? 'processing'} ${Math.round((activeJob.progress ?? 0) * 100)}%`
                : asset.status;
          return (
            <div
              key={asset.id}
              className="panel overflow-hidden group cursor-grab active:cursor-grabbing"
              draggable
              onDragStart={(e) => {
                e.dataTransfer.setData('text/cutboard-asset', asset.id);
                e.dataTransfer.effectAllowed = 'copy';
              }}
              onDoubleClick={() => void addAssetToTimeline(asset.id)}
              title={`${asset.originalName} — drag to timeline or double-click to add`}
            >
              <div className="aspect-video bg-surface-800 relative">
                {asset.thumbPath ? (
                  <img
                    src={mediaUrl(asset.thumbPath)}
                    alt={asset.originalName}
                    className="w-full h-full object-cover"
                    draggable={false}
                  />
                ) : (
                  <div className="w-full h-full flex items-center justify-center text-neutral-700 text-lg">
                    {asset.kind === 'audio' ? '♪' : asset.kind === 'image' ? '🖼' : '🎬'}
                  </div>
                )}
                {asset.kind !== 'image' && (
                  <span className="absolute bottom-1 right-1 chip bg-black/70 text-neutral-300">
                    {formatDuration(asset.durationMs)}
                  </span>
                )}
              </div>
              <div className="p-1.5">
                <p className="text-[11px] text-neutral-300 truncate">{asset.originalName}</p>
                <div className="mt-1 flex items-center gap-1">
                  {status === 'analyzed' && asset.error?.startsWith('Transcription') ? (
                    <span className="chip bg-amber-500/15 text-amber-400" title={asset.error}>
                      ✓ analyzed · no transcript
                    </span>
                  ) : status === 'analyzed' ? (
                    <span className="chip bg-accent/15 text-accent">✓ analyzed</span>
                  ) : status === 'missing' ? (
                    <MissingBadge assetId={asset.id} />
                  ) : status === 'failed' ? (
                    <span className="chip bg-red-500/15 text-red-400" title={asset.error ?? ''}>
                      failed
                    </span>
                  ) : (
                    <span className="chip bg-surface-700 text-neutral-400 capitalize">{status}</span>
                  )}
                </div>
              </div>
            </div>
          );
        })}
        {filtered.length === 0 && (
          <div className="col-span-2 text-center text-xs text-neutral-600 py-10">
            <p className="mb-3">No footage yet.</p>
            <button className="btn-outline mx-auto" onClick={() => void importMedia()}>
              Import media
            </button>
          </div>
        )}
      </div>
    </div>
  );
}

function MissingBadge({ assetId }: { assetId: string }) {
  const relink = async () => {
    const newPath = await window.cutboard.pickFilePath();
    if (!newPath) return;
    try {
      await window.cutboard.relinkAsset(assetId, newPath);
      useEditor.getState().showToast('Asset relinked.');
    } catch (err) {
      useEditor.getState().showToast(`Relink failed: ${err instanceof Error ? err.message : String(err)}`);
    }
  };
  return (
    <button className="chip bg-amber-500/15 text-amber-400 hover:bg-amber-500/25" onClick={() => void relink()}>
      missing — relink
    </button>
  );
}

function formatDuration(ms: number): string {
  const s = Math.round(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
