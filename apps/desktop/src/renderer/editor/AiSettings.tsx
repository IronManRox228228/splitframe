import { useState } from 'react';
import { useEditor } from '../store.ts';
import { Dialog } from '../ui/Dialog.tsx';
import { ModelsSection } from './ModelsSection.tsx';

export interface AiConfig {
  agentProvider: string;
  agentModel: string;
  asrModel: string;
  vlmProvider: string;
  llamacppUrl: string;
  ollamaUrl: string;
}

export const PROVIDER_LABELS: Record<string, string> = {
  anthropic: 'Anthropic',
  openai: 'OpenAI',
  google: 'Google',
  ollama: 'Ollama',
  llamacpp: 'llama.cpp',
};

/** Provider, model, server URL, API key and local models, in a dialog instead of squashing the chat. */
export function AiSettings({ config, onSaved, onClose }: { config: AiConfig; onSaved(next: AiConfig): void; onClose(): void }) {
  const [draft, setDraft] = useState(config);
  const [apiKey, setApiKey] = useState('');
  const [saving, setSaving] = useState(false);
  const isLocalServer = draft.agentProvider === 'llamacpp' || draft.agentProvider === 'ollama';

  const save = () => {
    setSaving(true);
    void window.cutboard
      .aiSetConfig({
        agentProvider: draft.agentProvider,
        agentModel: draft.agentModel,
        ...(draft.agentProvider === 'ollama' && draft.ollamaUrl.trim() ? { ollamaUrl: draft.ollamaUrl.trim() } : {}),
        ...(draft.agentProvider === 'llamacpp' && draft.llamacppUrl.trim() ? { llamacppUrl: draft.llamacppUrl.trim() } : {}),
        ...(apiKey && draft.agentProvider === 'llamacpp' ? { llamacppKey: apiKey } : {}),
        ...(apiKey && draft.agentProvider !== 'llamacpp' && draft.agentProvider !== 'ollama' ? { agentKey: apiKey } : {}),
      })
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
      </div>
    </Dialog>
  );
}
