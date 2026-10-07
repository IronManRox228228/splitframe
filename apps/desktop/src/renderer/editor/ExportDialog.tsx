import { useEffect, useMemo, useRef, useState } from 'react';
import { docDurationFrames } from '@cutboard/editor-core';
import { useEditor, type ExportRowInfo } from '../store.ts';
import { Dialog } from '../ui/Dialog.tsx';
import { Icon } from '../ui/Icon.tsx';
import { Segmented } from '../ui/Segmented.tsx';
import { aspectLabel, formatDuration } from '../home/format.ts';
import { mediaUrl } from '../lib/url.ts';
import {
  EXPORT_QUALITIES,
  estimateBytes,
  formatBytes,
  resolveExport,
  sanitizeFileName,
  type ExportFormat,
  type ExportQuality,
} from '../../shared/export-options.ts';

type Preset = { name: string; width: number; height: number; format: string; videoBitrateK: number };
const CUSTOM = '__custom__';
const FORMAT_OPTIONS: { value: ExportFormat; label: string }[] = [
  { value: 'mp4', label: 'MP4' },
  { value: 'webm', label: 'WebM' },
];
const QUALITY_OPTIONS: { value: ExportQuality; label: string }[] = EXPORT_QUALITIES.map((q) => ({ value: q, label: q }));

const RUNNING = ['queued', 'rendering', 'encoding'];

const plainError = (err: unknown): string =>
  err instanceof Error ? err.message.replace(/^Error invoking remote method '[^']+': (Error: )?/, '') : String(err);

const baseName = (path: string) => path.split(/[\\/]/).pop() ?? path;
const dirLabel = (path: string) => {
  const parts = path.split(/[\\/]/).filter(Boolean);
  return parts.length > 1 ? parts[parts.length - 2]! : 'the export folder';
};

export function ExportDialog() {
  const open = useEditor((s) => s.exportDialogOpen);
  const setExportDialog = useEditor((s) => s.setExportDialog);
  const watchedId = useEditor((s) => s.watchedExportId);
  const watchExport = useEditor((s) => s.watchExport);
  const exportsList = useEditor((s) => s.exportsList);
  const doc = useEditor((s) => s.doc);
  const assets = useEditor((s) => s.assets);
  const showToast = useEditor((s) => s.showToast);

  const [presets, setPresets] = useState<Preset[]>([]);
  const [folder, setFolder] = useState<{ dir: string; label: string } | null>(null);
  const [fileName, setFileName] = useState('');
  const [quality, setQuality] = useState<ExportQuality>('1080p');
  const [format, setFormat] = useState<ExportFormat>('mp4');
  const [presetName, setPresetName] = useState(CUSTOM);
  const [starting, setStarting] = useState(false);
  const [startError, setStartError] = useState<string | null>(null);
  const projectId = doc?.project.id;
  const projectName = doc?.project.name ?? '';

  // fresh form every time the dialog opens
  useEffect(() => {
    if (!open) return;
    setFileName(sanitizeFileName(projectName));
    setStartError(null);
    void window.cutboard.exportPresets().then(setPresets).catch(() => undefined);
    void window.cutboard.getExportFolder().then(setFolder).catch(() => undefined);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, projectId]);

  const session: ExportRowInfo | undefined = watchedId ? exportsList.find((e) => e.id === watchedId) : undefined;
  const running = Boolean(session && RUNNING.includes(session.status));
  const view: 'setup' | 'running' | 'done' | 'failed' =
    session?.status === 'done' ? 'done' : session?.status === 'failed' ? 'failed' : running ? 'running' : 'setup';

  const info = useMemo(() => {
    if (!doc) return null;
    const frames = docDurationFrames(doc);
    const fps = doc.project.fps;
    const clips = doc.items.filter((i) => i.type === 'video' || i.type === 'image' || i.type === 'audio').length;
    const captions = doc.items.filter((i) => i.type === 'caption').length;
    let poster: string | null = null;
    for (const item of [...doc.items].sort((a, b) => a.startFrame - b.startFrame)) {
      if (item.type !== 'video' && item.type !== 'image') continue;
      const thumb = assets.find((a) => a.id === item.assetId)?.thumbPath;
      if (thumb) {
        poster = thumb;
        break;
      }
    }
    return { frames, fps, seconds: frames / fps, clips, captions, poster, empty: doc.items.length === 0 || frames === 0 };
  }, [doc, assets]);

  const preset = presets.find((p) => p.name === presetName);
  const resolved = useMemo(() => {
    if (!doc) return null;
    if (preset) return { width: preset.width, height: preset.height, format: preset.format as ExportFormat, videoBitrateK: preset.videoBitrateK };
    return resolveExport(quality, format, doc.project.width, doc.project.height);
  }, [doc, preset, quality, format]);

  if (!open || !doc || !info || !resolved) return null;

  const close = () => {
    if (session && !running) watchExport(null); // a finished or failed run has been seen; start clean next time
    setExportDialog(false);
  };

  const cleanName = sanitizeFileName(fileName);
  const start = async () => {
    if (info.empty || !cleanName || starting) return;
    setStarting(true);
    setStartError(null);
    try {
      const row = (await window.cutboard.startExport(
        preset ? { presetName: preset.name, fileName: cleanName } : { quality, format, fileName: cleanName },
      )) as { id: string };
      watchExport(row.id);
    } catch (err) {
      setStartError(plainError(err));
    } finally {
      setStarting(false);
    }
  };

  const title = view === 'running' ? 'Exporting…' : view === 'done' ? 'Ready' : view === 'failed' ? 'Export failed' : 'Export';

  return (
    <Dialog title={title} onClose={close}>
      {view === 'setup' && (
        <div className="flex flex-col gap-5 px-6 pt-5 pb-6">
          <div className="flex items-center gap-3.5">
            <div className="w-[112px] h-[63px] shrink-0 rounded-xl bg-surface-850 overflow-hidden flex items-center justify-center shadow-[inset_0_0_0_1px_rgba(255,255,255,0.05)]">
              {info.poster ? (
                <img src={mediaUrl(info.poster)} alt="" className="w-full h-full object-cover" />
              ) : (
                <Icon name="media" size={22} strokeWidth={1.4} className="text-surface-600" />
              )}
            </div>
            <div className="flex flex-col gap-[3px] min-w-0">
              <span className="text-sm font-medium text-fg truncate">{projectName}</span>
              <span className="text-xs text-fg-muted">
                {formatDuration(info.seconds * 1000)} · {aspectLabel(doc.project.width, doc.project.height)} · {info.clips} {info.clips === 1 ? 'clip' : 'clips'}, {info.captions}{' '}
                {info.captions === 1 ? 'caption' : 'captions'}
              </span>
            </div>
          </div>

          <label className="flex flex-col gap-2">
            <span className="text-xs text-fg-2">File name</span>
            <span className="flex items-center h-10 px-3 rounded-xl bg-[rgba(8,10,9,0.4)] border border-line focus-within:border-accent/60 transition-colors">
              <input
                value={fileName}
                maxLength={120}
                onChange={(e) => setFileName(e.target.value)}
                aria-label="File name"
                className="flex-1 min-w-0 bg-transparent outline-none text-[13px] text-fg"
              />
              <span className="text-[13px] text-fg-faint">.{resolved.format}</span>
            </span>
          </label>

          <div className="flex flex-col gap-2">
            <span className="text-xs text-fg-2">Save to</span>
            <button
              type="button"
              onClick={() => void window.cutboard.chooseExportFolder().then((f) => f && setFolder(f)).catch((err) => showToast(plainError(err), { kind: 'error' }))}
              className="h-10 px-3 rounded-xl bg-[rgba(8,10,9,0.4)] border border-line hover:border-line-strong flex items-center gap-2.5 text-[13px] text-fg transition-colors"
              title={folder?.dir}
            >
              <Icon name="folder" size={16} className="text-fg-2 shrink-0" />
              <span className="flex-1 text-left truncate">{folder?.label ?? '…'}</span>
              <span className="text-xs text-fg-muted">Change</span>
            </button>
          </div>

          <div className="grid grid-cols-2 gap-4">
            <div className="flex flex-col gap-2">
              <span className="text-xs text-fg-2">Quality</span>
              <div className={preset ? 'opacity-40 pointer-events-none' : ''} aria-disabled={Boolean(preset)}>
                <Segmented<ExportQuality> label="Quality" value={quality} options={QUALITY_OPTIONS} onChange={setQuality} />
              </div>
            </div>
            <div className="flex flex-col gap-2">
              <span className="text-xs text-fg-2">Format</span>
              <div className={preset ? 'opacity-40 pointer-events-none' : ''} aria-disabled={Boolean(preset)}>
                <Segmented<ExportFormat> label="Format" value={format} options={FORMAT_OPTIONS} onChange={setFormat} />
              </div>
            </div>
          </div>

          <label className="flex flex-col gap-2">
            <span className="text-xs text-fg-2">Preset</span>
            <select value={presetName} onChange={(e) => setPresetName(e.target.value)} className="input h-10 text-[13px]" aria-label="Preset">
              <option value={CUSTOM}>Match this video (use Quality and Format)</option>
              {presets.map((p) => (
                <option key={p.name} value={p.name}>
                  {p.name} — {p.width}×{p.height} {p.format.toUpperCase()}
                </option>
              ))}
            </select>
          </label>

          {info.empty && <p className="text-[13px] text-fg-2 rounded-xl bg-[rgba(8,10,9,0.4)] border border-line px-3 py-2.5">Your timeline is empty. Add a clip before exporting.</p>}
          {startError && <p className="text-[13px] text-danger">{startError}</p>}

          <div className="flex items-center justify-between gap-4 pt-1">
            <span className="text-xs text-fg-muted">
              About {formatBytes(estimateBytes(resolved.videoBitrateK, info.seconds))} · {resolved.width}×{resolved.height} · {info.fps} fps · {resolved.format === 'webm' ? 'VP9' : 'H.264'}
            </span>
            <button className="btn-primary h-10 px-5" disabled={info.empty || !cleanName || starting} onClick={() => void start()}>
              {starting ? 'Starting…' : 'Export video'}
            </button>
          </div>

          <button
            type="button"
            className="self-start text-xs text-fg-muted hover:text-fg transition-colors"
            onClick={() =>
              void window.cutboard
                .exportOtio()
                .then(({ path }) =>
                  showToast('Timeline saved for Resolve, Premiere and Final Cut', {
                    kind: 'success',
                    action: { label: 'Show in folder', run: () => void window.cutboard.revealExportPath(path) },
                  }),
                )
                .catch((err) => showToast(`Could not export the timeline: ${plainError(err)}`, { kind: 'error' }))
            }
          >
            Export timeline for Resolve / Premiere (OTIO)
          </button>
        </div>
      )}

      {view === 'running' && session && <RunningView session={session} onCancel={() => void window.cutboard.cancelExport(session.id)} onKeepEditing={close} />}

      {view === 'done' && session && (
        <div className="flex flex-col gap-[18px] px-6 pt-5 pb-6">
          <div className="flex items-center gap-3.5">
            <span className="w-10 h-10 rounded-full bg-success/15 text-success flex items-center justify-center shrink-0">
              <Icon name="check" size={18} strokeWidth={2.2} />
            </span>
            <div className="flex flex-col gap-[3px] min-w-0">
              <span className="text-sm font-medium text-fg truncate">{baseName(session.outputPath ?? '')}</span>
              <span className="text-xs text-fg-muted">
                {[
                  session.sizeBytes ? formatBytes(session.sizeBytes) : null,
                  `saved to ${dirLabel(session.outputPath ?? '')}`,
                  tookLabel(session),
                ]
                  .filter(Boolean)
                  .join(' · ')}
              </span>
            </div>
          </div>
          <div className="flex justify-end gap-2">
            <button className="btn-outline h-10 px-4" onClick={() => void window.cutboard.revealExportPath(session.outputPath ?? '')}>
              Show in folder
            </button>
            <button
              className="btn-primary h-10 px-4"
              onClick={() =>
                void window.cutboard.openExportedFile(session.outputPath ?? '').then((ok) => {
                  if (!ok) showToast('Could not open the video. It may have been moved or deleted.', { kind: 'error' });
                })
              }
            >
              Open video
            </button>
          </div>
        </div>
      )}

      {view === 'failed' && session && (
        <div className="flex flex-col gap-[18px] px-6 pt-5 pb-6">
          <div className="flex items-start gap-3.5">
            <span className="w-10 h-10 rounded-full bg-danger/15 text-danger flex items-center justify-center shrink-0">
              <Icon name="alert" size={18} />
            </span>
            <div className="flex flex-col gap-[3px] min-w-0">
              <span className="text-sm font-medium text-fg">The export did not finish</span>
              <span className="text-xs text-fg-muted break-words">{summarizeError(session.error)}</span>
            </div>
          </div>
          <div className="flex justify-end gap-2">
            <button
              className="btn-outline h-10 px-4"
              onClick={() =>
                void navigator.clipboard
                  .writeText(session.error ?? 'Unknown error')
                  .then(() => showToast('Details copied'))
                  .catch(() => showToast('Could not copy the details', { kind: 'error' }))
              }
            >
              <Icon name="copy" size={14} />
              Copy details
            </button>
            <button className="btn-primary h-10 px-4" onClick={() => watchExport(null)}>
              Try again
            </button>
          </div>
        </div>
      )}
    </Dialog>
  );
}

function summarizeError(error: string | null): string {
  const first = (error ?? 'Something went wrong while exporting.').split('\n')[0]!.trim();
  return first.length > 140 ? `${first.slice(0, 137)}…` : first;
}

function tookLabel(row: ExportRowInfo): string | null {
  if (!row.createdAt || !row.updatedAt) return null;
  const sec = Math.max(1, Math.round((Date.parse(row.updatedAt) - Date.parse(row.createdAt)) / 1000));
  return sec < 60 ? `took ${sec} s` : `took ${Math.floor(sec / 60)} min ${sec % 60} s`;
}

function RunningView({ session, onCancel, onKeepEditing }: { session: ExportRowInfo; onCancel(): void; onKeepEditing(): void }) {
  const pct = Math.min(100, Math.round(session.progress * 100));
  // progress is a share of the whole job, so elapsed / progress gives the total
  const startedAt = useRef(session.createdAt ? Date.parse(session.createdAt) : Date.now());
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  const elapsedSec = Math.max(0, (now - startedAt.current) / 1000);
  const etaSec = session.progress >= 0.04 && elapsedSec > 1 ? Math.max(1, Math.round((elapsedSec / session.progress) * (1 - session.progress))) : null;
  const frames = session.frames;
  const fps = frames && elapsedSec > 1 ? Math.round(frames.done / elapsedSec) : null;
  const detail =
    session.status === 'queued'
      ? 'Getting ready…'
      : session.status === 'encoding'
        ? 'Finishing the video file…'
        : frames && frames.total > 0
          ? `Rendering frame ${Math.min(frames.done, frames.total)} of ${frames.total}${fps ? ` · ${fps} fps` : ''}`
          : 'Rendering…';
  return (
    <div className="flex flex-col gap-[18px] px-6 pt-5 pb-6">
      <div className="flex items-baseline justify-between">
        <span className="font-mono text-[28px] font-medium text-fg tabular-nums">{pct}%</span>
        <span className="text-xs text-fg-muted">{etaSec === null ? 'Estimating time…' : `About ${etaSec < 90 ? `${etaSec} s` : `${Math.ceil(etaSec / 60)} min`} left`}</span>
      </div>
      <div className="h-1.5 rounded-full bg-fg/15" role="progressbar" aria-valuenow={pct} aria-valuemin={0} aria-valuemax={100} aria-label="Export progress">
        <div className="h-1.5 rounded-full bg-accent transition-[width] duration-300" style={{ width: `${pct}%` }} />
      </div>
      <span className="text-xs text-fg-muted">{detail}</span>
      <div className="flex justify-end gap-2">
        <button className="btn-outline h-10 px-4" onClick={onCancel}>
          Cancel
        </button>
        <button className="btn-primary h-10 px-4" onClick={onKeepEditing}>
          Keep editing
        </button>
      </div>
    </div>
  );
}
