import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';

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
  error?: string;
}

/**
 * Local models. Nothing is downloaded unless the user presses Download here: transcription
 * models come from the whisper.cpp repository and the semantic-search model from
 * huggingface.co, both explicit, one-off downloads.
 */
export function ModelsSection() {
  const [models, setModels] = useState<ModelRow[]>([]);
  const [busy, setBusy] = useState<Record<string, Progress>>({});

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
      if (p.done || p.error) refresh();
    });
  }, []);

  const download = (id: string) => {
    setBusy((prev) => ({ ...prev, [id]: { received: 0, total: 0 } }));
    void window.cutboard.downloadModel(id).then((res) => {
      setBusy((prev) => {
        const next = { ...prev };
        delete next[id];
        return next;
      });
      if (!res.ok) useEditor.getState().showToast(`Download failed: ${res.error ?? 'unknown error'}`, { kind: 'error' });
      refresh();
    });
  };

  return (
    <div className="space-y-2.5">
      <p className="label">Local models (downloaded only when you ask)</p>
      {models.map((m) => {
        const progress = busy[m.id];
        return (
          <div key={m.id} className="flex items-center gap-2">
            <div className="flex-1 min-w-0">
              <p className="text-sm text-fg-2 truncate">{m.id}</p>
              <p className="text-[11px] text-fg-muted truncate">
                {m.kind === 'asr' ? 'transcription' : 'semantic search'}
                {m.sizeMB ? ` · ${m.sizeMB} MB` : ''}
              </p>
            </div>
            {m.downloaded ? (
              <span className="text-success text-[11px]">Installed</span>
            ) : progress ? (
              <span className="text-accent text-[11px]">
                {progress.total > 0 ? `${Math.round((progress.received / progress.total) * 100)}%` : '…'}
              </span>
            ) : (
              <button className="btn-outline btn-sm" onClick={() => download(m.id)}>
                Download
              </button>
            )}
          </div>
        );
      })}
    </div>
  );
}
