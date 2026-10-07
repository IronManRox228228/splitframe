import { useEffect, useState } from 'react';
import { Dialog } from '../ui/Dialog.tsx';

interface ModelRow {
  id: string;
  kind: 'asr' | 'embeddings';
  downloaded: boolean;
  sizeMB?: number;
  note?: string;
}

interface Progress {
  received: number;
  total: number;
}

const mb = (bytes: number): number => Math.round(bytes / (1024 * 1024));

/**
 * Local models. Nothing is downloaded unless the user presses Download here: transcription
 * models come from the whisper.cpp repository and the semantic-search model from
 * huggingface.co, both explicit, one-off downloads.
 */
export function ModelsSection() {
  const [models, setModels] = useState<ModelRow[]>([]);
  const [busy, setBusy] = useState<Record<string, Progress>>({});
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [confirmDelete, setConfirmDelete] = useState<ModelRow | null>(null);

  const refresh = () => void window.cutboard.listModels().then((m) => setModels(m as ModelRow[]));

  useEffect(() => {
    refresh();
    return window.cutboard.onEvent((envelope) => {
      if (envelope.type !== 'model:progress') return;
      const p = envelope.payload as { id: string; received: number; total: number; done: boolean; error?: string };
      setBusy((prev) => {
        const next = { ...prev };
        if (p.done || p.error) delete next[p.id];
        else next[p.id] = { received: p.received, total: p.total };
        return next;
      });
      // a user cancel is not an error worth showing
      if (p.error && p.error !== 'cancelled') setErrors((prev) => ({ ...prev, [p.id]: p.error! }));
      if (p.done || p.error) refresh();
    });
  }, []);

  const download = (id: string) => {
    setErrors((prev) => {
      const next = { ...prev };
      delete next[id];
      return next;
    });
    setBusy((prev) => ({ ...prev, [id]: { received: 0, total: 0 } }));
    void window.cutboard
      .downloadModel(id)
      .then((res) => {
        setBusy((prev) => {
          const next = { ...prev };
          delete next[id];
          return next;
        });
        if (!res.ok && res.error && res.error !== 'cancelled') setErrors((prev) => ({ ...prev, [id]: res.error! }));
      })
      .catch((err) => {
        setBusy((prev) => {
          const next = { ...prev };
          delete next[id];
          return next;
        });
        setErrors((prev) => ({ ...prev, [id]: err instanceof Error ? err.message : String(err) }));
      })
      .finally(refresh);
  };

  const remove = (m: ModelRow) => {
    setConfirmDelete(null);
    void window.cutboard
      .deleteModel(m.id)
      .catch((err) => setErrors((prev) => ({ ...prev, [m.id]: err instanceof Error ? err.message : String(err) })))
      .finally(refresh);
  };

  return (
    <div className="space-y-3">
      <p className="label">Local models (downloaded only when you ask)</p>
      {models.map((m) => {
        const progress = busy[m.id];
        const error = errors[m.id];
        const pct = progress && progress.total > 0 ? Math.min(100, Math.round((progress.received / progress.total) * 100)) : null;
        return (
          <div key={m.id} className="flex flex-col gap-1.5">
            <div className="flex items-center gap-2">
              <div className="flex-1 min-w-0">
                <p className="text-sm text-fg-2 truncate">{m.id}</p>
                <p className="text-[11px] text-fg-muted truncate">
                  {m.kind === 'asr' ? 'transcription' : 'semantic search'}
                  {m.sizeMB ? ` · ${m.sizeMB} MB` : ''}
                </p>
              </div>
              {progress ? (
                m.kind === 'asr' && (
                  <button className="btn-outline btn-sm" onClick={() => void window.cutboard.cancelModelDownload(m.id)}>
                    Cancel
                  </button>
                )
              ) : m.downloaded ? (
                <>
                  <span className="text-success text-[11px]">Installed</span>
                  {m.kind === 'asr' && (
                    <button className="btn-outline btn-sm" onClick={() => setConfirmDelete(m)}>
                      Delete
                    </button>
                  )}
                </>
              ) : (
                <button className="btn-outline btn-sm" onClick={() => download(m.id)}>
                  {error ? 'Retry' : 'Download'}
                </button>
              )}
            </div>
            {progress && (
              <div className="flex flex-col gap-1">
                <div
                  className="h-1.5 rounded-full bg-fg/15 overflow-hidden"
                  role="progressbar"
                  aria-label={`Downloading ${m.id}`}
                  aria-valuenow={pct ?? undefined}
                  aria-valuemin={0}
                  aria-valuemax={100}
                >
                  <div className={`h-full bg-accent ${pct === null ? 'w-1/3 animate-pulse' : ''}`} style={pct === null ? undefined : { width: `${Math.max(2, pct)}%` }} />
                </div>
                <p className="text-[11px] text-fg-muted">
                  {progress.total > 0 ? `${mb(progress.received)} / ${mb(progress.total)} MB` : 'Starting download…'}
                </p>
              </div>
            )}
            {error && !progress && !m.downloaded && (
              <p className="text-[11px] text-danger leading-snug" role="alert">
                Download failed: {error}
              </p>
            )}
          </div>
        );
      })}
      {confirmDelete && (
        <Dialog title="Delete this model?" width={420} onClose={() => setConfirmDelete(null)}>
          <div className="px-6 pt-3 pb-6 flex flex-col gap-5">
            <p className="text-sm text-fg-2">
              {confirmDelete.id} will be removed from this computer{confirmDelete.sizeMB ? ` (frees ${confirmDelete.sizeMB} MB)` : ''}. Transcription won&rsquo;t work with it until you download it again.
            </p>
            <div className="flex justify-end gap-2">
              <button className="btn-outline" onClick={() => setConfirmDelete(null)}>
                Cancel
              </button>
              <button className="btn-danger" onClick={() => remove(confirmDelete)}>
                Delete model
              </button>
            </div>
          </div>
        </Dialog>
      )}
    </div>
  );
}
