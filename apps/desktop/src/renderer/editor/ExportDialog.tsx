import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';
import { revealLabel } from '../lib/platform-labels.ts';

export function ExportDialog() {
  const open = useEditor((s) => s.exportDialogOpen);
  const setExportDialog = useEditor((s) => s.setExportDialog);
  const doc = useEditor((s) => s.doc);
  const platform = useEditor((s) => s.appInfo?.platform);
  const exportsList = useEditor((s) => s.exportsList);
  const [presets, setPresets] = useState<{ name: string; width: number; height: number; format: string }[]>([]);
  const [selected, setSelected] = useState<string | null>(null);
  const [starting, setStarting] = useState(false);

  useEffect(() => {
    if (open) {
      void window.cutboard.exportPresets().then((p) => {
        setPresets(p);
        setSelected((s) => s ?? p[0]?.name ?? null);
      });
    }
  }, [open]);

  if (!open || !doc) return null;
  const active = exportsList.find((e) => e.status === 'rendering' || e.status === 'encoding' || e.status === 'queued');
  // newest first: the outcome of the export that just ended (done / failed / cancelled)
  const latest = !active ? exportsList[0] : undefined;

  const start = async () => {
    if (!selected) return;
    setStarting(true);
    try {
      await window.cutboard.startExport(selected);
    } catch (err) {
      useEditor.getState().showToast(err instanceof Error ? err.message : String(err));
    } finally {
      setStarting(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 bg-black/60 flex items-center justify-center" onClick={() => !active && setExportDialog(false)}>
      <div className="panel w-[480px] p-5" onClick={(e) => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-sm font-semibold text-fg">Export</h2>
          <button className="btn-ghost w-7 px-0 justify-center" onClick={() => !active && setExportDialog(false)}>
            ✕
          </button>
        </div>

        <div className="grid grid-cols-2 gap-2 mb-4">
          {presets.map((p) => (
            <button
              key={p.name}
              disabled={Boolean(active)}
              onClick={() => setSelected(p.name)}
              className={`text-left rounded-lg border px-3 py-2.5 transition-colors ${
                selected === p.name ? 'border-accent bg-accent/10' : 'border-line hover:border-surface-600'
              } disabled:opacity-50`}
            >
              <p className="text-xs font-medium text-fg">{p.name}</p>
              <p className="text-[11px] text-fg-faint">
                {p.width}×{p.height} · {p.format.toUpperCase()}
              </p>
            </button>
          ))}
        </div>

        {latest?.status === 'done' && (
          <div className="mb-3 flex items-center justify-between rounded border border-emerald-500/30 bg-emerald-500/10 px-3 py-2">
            <span className="text-[11px] text-emerald-400">Export finished ✓</span>
            <button className="btn-outline" onClick={() => void window.cutboard.revealExportPath(latest.outputPath ?? '')}>
              {revealLabel(platform)}
            </button>
          </div>
        )}
        {latest?.status === 'failed' && (
          <p className="mb-3 rounded border border-red-500/30 bg-red-500/10 px-3 py-2 text-[11px] text-red-400">
            Export failed: {latest.error ?? 'unknown error'}
          </p>
        )}

        {active ? (
          <div className="mb-2">
            <div className="flex justify-between text-[11px] text-fg-muted mb-1.5">
              <span className="capitalize">{active.status}…</span>
              <span>{Math.round(active.progress * 100)}%</span>
            </div>
            <div className="h-1.5 bg-surface-700 rounded-full overflow-hidden">
              <div className="h-full bg-accent transition-all" style={{ width: `${active.progress * 100}%` }} />
            </div>
            <button className="btn-outline mt-3" onClick={() => void window.cutboard.cancelExport(active.id)}>
              Cancel export
            </button>
          </div>
        ) : (
          <div className="flex items-center justify-between">
            <p className="text-[11px] text-fg-faint">
              Frame-accurate render via the preview compositor, encoded with your OS hardware encoder.
            </p>
            <button className="btn-primary" disabled={!selected || starting} onClick={() => void start()}>
              {starting ? 'Starting…' : 'Start export'}
            </button>
          </div>
        )}

        {exportsList.length > 0 && !active && (
          <div className="mt-4 border-t border-line pt-3">
            <p className="text-[11px] uppercase tracking-wide text-fg-faint mb-1.5">Recent exports</p>
            <ul className="space-y-1">
              {exportsList.slice(0, 4).map((e) => (
                <li key={e.id} className="flex items-center justify-between text-[11px]">
                  <button className="text-fg-muted truncate hover:text-fg-2" onClick={() => void window.cutboard.revealExportPath(e.outputPath ?? '')}>
                    {e.preset.name}
                  </button>
                  <span className={e.status === 'done' ? 'text-emerald-400' : e.status === 'failed' ? 'text-red-400' : 'text-fg-faint'}>
                    {e.status}
                  </span>
                </li>
              ))}
            </ul>
          </div>
        )}

        <button
          className="btn-outline w-full mt-4"
          onClick={() =>
            void window.cutboard.exportOtio().then(({ path }) => {
              useEditor.getState().showToast(`Timeline exported for Resolve/Premiere: ${path}`);
            })
          }
        >
          Export timeline as OTIO (Resolve / Premiere / FCP)
        </button>
      </div>
    </div>
  );
}
