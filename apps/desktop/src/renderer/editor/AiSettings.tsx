import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';
import { Dialog } from '../ui/Dialog.tsx';
import { ModelsSection } from './ModelsSection.tsx';

export interface AiConfig {
  agentProvider: string;
  agentModel: string;
  harness?: string;
  plannerModel?: string;
  executorModel?: string;
  asrModel: string;
  vlmProvider: string;
  llamacppUrl: string;
  ollamaUrl: string;
  meterlessUrl?: string;
}

export const PROVIDER_LABELS: Record<string, string> = {
  anthropic: 'Anthropic',
  openai: 'OpenAI',
  google: 'Google',
  meterless: 'Meterless',
  ollama: 'Ollama',
  llamacpp: 'llama.cpp',
};

/** Provider, model, server URL, API key and local models, in a dialog instead of squashing the chat. */
export function AiSettings({ config, onSaved, onClose }: { config: AiConfig; onSaved(next: AiConfig): void; onClose(): void }) {
  const [draft, setDraft] = useState(config);
  const [apiKey, setApiKey] = useState('');
  const [saving, setSaving] = useState(false);
  const [zetarisDraft, setZetarisDraft] = useState({
    enabled: true,
    endpoint: 'https://api.zetaris.com/v1/federation/query',
    catalogName: 'production_media_catalog',
  });
  const [zetarisKey, setZetarisKey] = useState('');
  const isLocalServer = draft.agentProvider === 'llamacpp' || draft.agentProvider === 'ollama';

  useEffect(() => {
    void window.cutboard?.zetarisGetConfig?.().then((cfg) => {
      if (cfg) setZetarisDraft(cfg);
    });
  }, []);

  const save = () => {
    setSaving(true);
    const pAi = window.cutboard.aiSetConfig({
      agentProvider: draft.agentProvider,
      agentModel: draft.agentModel,
      harness: draft.harness === 'classic' ? 'classic' : 'v1',
      plannerModel: draft.plannerModel ?? '',
      executorModel: draft.executorModel ?? '',
      ...(draft.agentProvider === 'ollama' && draft.ollamaUrl.trim() ? { ollamaUrl: draft.ollamaUrl.trim() } : {}),
      ...(draft.agentProvider === 'llamacpp' && draft.llamacppUrl.trim() ? { llamacppUrl: draft.llamacppUrl.trim() } : {}),
      ...(draft.agentProvider === 'meterless' && draft.meterlessUrl?.trim() ? { meterlessUrl: draft.meterlessUrl.trim() } : {}),
      ...(apiKey && draft.agentProvider === 'llamacpp' ? { llamacppKey: apiKey } : {}),
      ...(apiKey && draft.agentProvider !== 'llamacpp' && draft.agentProvider !== 'ollama' ? { agentKey: apiKey } : {}),
    });
    const pZetaris = window.cutboard.zetarisSetConfig({
      enabled: zetarisDraft.enabled,
      endpoint: zetarisDraft.endpoint.trim(),
      catalogName: zetarisDraft.catalogName.trim(),
      ...(zetarisKey.trim() ? { apiKey: zetarisKey.trim() } : {}),
    });

    void Promise.all([pAi, pZetaris])
      .then(() => {
        useEditor.getState().showToast('AI settings saved.', { kind: 'success' });
        onSaved(draft);
        onClose();
      })
      .catch(() => {
        setSaving(false);
        useEditor.getState().showToast('Could not save AI settings. Check the server URL.', { kind: 'error' });
      });
  };

  return (
    <Dialog title="AI settings" onClose={onClose} width={480}>
      <div className="p-6 pt-4 flex flex-col gap-4">
        <label className="flex flex-col gap-1.5">
          <span className="label">Provider</span>
          <select
            value={draft.agentProvider}
            onChange={(e) => setDraft({ ...draft, agentProvider: e.target.value })}
            className="input"
          >
            <option value="anthropic">Anthropic (cloud)</option>
            <option value="openai">OpenAI (cloud)</option>
            <option value="google">Google (cloud)</option>
            <option value="meterless">Meterless (serverless)</option>
            <option value="ollama">Ollama (local)</option>
            <option value="llamacpp">llama.cpp server (local)</option>
          </select>
        </label>
        <label className="flex flex-col gap-1.5">
          <span className="label">Model (optional override)</span>
          <input
            value={draft.agentModel}
            onChange={(e) => setDraft({ ...draft, agentModel: e.target.value })}
            placeholder={draft.agentProvider === 'llamacpp' ? 'Blank = whatever llama-server loaded' : 'e.g. claude-sonnet-4-5'}
            className="input"
          />
        </label>
        <label className="flex flex-col gap-1.5">
          <span className="label">Agent harness</span>
          <select value={draft.harness === 'classic' ? 'classic' : 'v1'} onChange={(e) => setDraft({ ...draft, harness: e.target.value })} className="input">
            <option value="v1">v1: plans, deterministic edits, verified results (local / OpenAI-compatible models)</option>
            <option value="classic">Classic: one tool loop</option>
          </select>
        </label>
        {draft.harness !== 'classic' && (
          <div className="grid grid-cols-2 gap-3">
            <label className="flex flex-col gap-1.5">
              <span className="label">Planner model</span>
              <input value={draft.plannerModel ?? ''} onChange={(e) => setDraft({ ...draft, plannerModel: e.target.value })} placeholder="Blank = agent model" className="input" />
            </label>
            <label className="flex flex-col gap-1.5">
              <span className="label">Executor model</span>
              <input value={draft.executorModel ?? ''} onChange={(e) => setDraft({ ...draft, executorModel: e.target.value })} placeholder="Blank = agent model" className="input" />
            </label>
          </div>
        )}
        {draft.agentProvider === 'meterless' && (
          <label className="flex flex-col gap-1.5">
            <span className="label">Endpoint URL</span>
            <input
              value={draft.meterlessUrl ?? ''}
              onChange={(e) => setDraft({ ...draft, meterlessUrl: e.target.value })}
              placeholder="https://api.meterless.ai/v1"
              className="input"
            />
            <span className="text-xs text-fg-muted">
              Meterless OpenAI-compatible serverless inference endpoint. Default: <code className="font-mono text-fg-2">https://api.meterless.ai/v1</code>
            </span>
          </label>
        )}
        {draft.agentProvider === 'ollama' && (
          <label className="flex flex-col gap-1.5">
            <span className="label">Server URL</span>
            <input
              value={draft.ollamaUrl}
              onChange={(e) => setDraft({ ...draft, ollamaUrl: e.target.value })}
              placeholder="http://127.0.0.1:11434"
              className="input"
            />
          </label>
        )}
        {draft.agentProvider === 'llamacpp' && (
          <label className="flex flex-col gap-1.5">
            <span className="label">Server URL</span>
            <input
              value={draft.llamacppUrl}
              onChange={(e) => setDraft({ ...draft, llamacppUrl: e.target.value })}
              placeholder="http://127.0.0.1:8080"
              className="input"
            />
            <span className="text-xs text-fg-muted">
              Start llama-server with <code className="font-mono text-fg-2">--jinja</code> or the assistant can't call editing tools.
            </span>
          </label>
        )}
        {draft.agentProvider !== 'ollama' && (
          <label className="flex flex-col gap-1.5">
            <span className="label">{draft.agentProvider === 'llamacpp' ? 'API key (only if started with --api-key)' : 'API key (stored encrypted)'}</span>
            <input value={apiKey} onChange={(e) => setApiKey(e.target.value)} type="password" placeholder="sk-…" className="input" autoComplete="off" />
          </label>
        )}
        <div className="flex justify-end gap-2">
          <button className="btn-ghost" onClick={onClose}>
            Cancel
          </button>
          <button className="btn-primary" onClick={save} disabled={saving}>
            Save
          </button>
        </div>
        <div className="border-t border-line pt-4">
          <ModelsSection />
          {isLocalServer && <p className="mt-3 text-xs text-fg-muted">Transcription and search models below are separate from the chat model above.</p>}
        </div>
        <div className="border-t border-line pt-4 flex flex-col gap-3">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold text-fg uppercase tracking-wider">Federated Catalog (Zetaris)</span>
            <label className="flex items-center gap-2 cursor-pointer text-xs text-fg-muted">
              <input
                type="checkbox"
                checked={zetarisDraft.enabled}
                onChange={(e) => setZetarisDraft({ ...zetarisDraft, enabled: e.target.checked })}
                className="rounded border-line bg-surface"
              />
              Enable
            </label>
          </div>
          {zetarisDraft.enabled && (
            <>
              <label className="flex flex-col gap-1.5">
                <span className="label">Federation Endpoint</span>
                <input
                  value={zetarisDraft.endpoint}
                  onChange={(e) => setZetarisDraft({ ...zetarisDraft, endpoint: e.target.value })}
                  placeholder="https://api.zetaris.com/v1/federation/query"
                  className="input"
                />
              </label>
              <label className="flex flex-col gap-1.5">
                <span className="label">Catalog Name</span>
                <input
                  value={zetarisDraft.catalogName}
                  onChange={(e) => setZetarisDraft({ ...zetarisDraft, catalogName: e.target.value })}
                  placeholder="production_media_catalog"
                  className="input"
                />
              </label>
              <label className="flex flex-col gap-1.5">
                <span className="label">API key (stored encrypted)</span>
                <input
                  value={zetarisKey}
                  onChange={(e) => setZetarisKey(e.target.value)}
                  type="password"
                  placeholder="zts_live_…"
                  className="input"
                  autoComplete="off"
                />
              </label>
            </>
          )}
        </div>
      </div>
    </Dialog>
  );
}
