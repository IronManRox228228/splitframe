import { describe, expect, it, vi } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import { AiSettings, PROVIDER_LABELS, type AiConfig } from './AiSettings.tsx';

// Mock dialog and models section so the test runs in pure Node without Electron dependencies
vi.mock('../ui/Dialog.tsx', () => ({
  Dialog: ({ title, children }: { title: string; children: React.ReactNode }) => (
    <div data-testid="dialog" data-title={title}>
      {children}
    </div>
  ),
}));

vi.mock('./ModelsSection.tsx', () => ({
  ModelsSection: () => <div data-testid="models-section">Models</div>,
}));

describe('AiSettings UI component', () => {
  const baseConfig: AiConfig = {
    agentProvider: 'anthropic',
    agentModel: 'claude-3-5-haiku-latest',
    asrModel: 'base.en',
    vlmProvider: 'none',
    llamacppUrl: 'http://127.0.0.1:8080',
    ollamaUrl: 'http://127.0.0.1:11434',
  };

  it('lists the vendor-neutral providers in the dropdown', () => {
    const html = renderToStaticMarkup(
      <AiSettings config={baseConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    expect(html).toContain('value="anthropic"');
    expect(html).toContain('Anthropic (cloud)</option>');
    expect(html).toContain('value="openai">OpenAI (cloud)</option>');
    expect(html).toContain('value="google">Google (cloud)</option>');
    expect(html).toContain('value="ollama">Ollama (local)</option>');
    expect(html).toContain('value="llamacpp">llama.cpp server (local)</option>');
    // the sponsor provider is gone in every form
    expect(html).not.toContain('meterless');
    expect(html).not.toContain('Meterless');
    expect(html).not.toContain('zetaris');
    expect(html).not.toContain('Zetaris');
  });

  it('shows the API key field and model placeholder for cloud providers', () => {
    const html = renderToStaticMarkup(
      <AiSettings config={baseConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    expect(html).toContain('placeholder="e.g. claude-sonnet-4-5"');
    expect(html).toContain('API key (stored encrypted)');
  });

  it('renders the Ollama server URL field when ollama is selected', () => {
    const ollamaConfig: AiConfig = {
      ...baseConfig,
      agentProvider: 'ollama',
    };

    const html = renderToStaticMarkup(
      <AiSettings config={ollamaConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    expect(html).toContain('Server URL');
    expect(html).toContain('placeholder="http://127.0.0.1:11434"');
  });

  it('preserves existing UI styling classes and structure', () => {
    const html = renderToStaticMarkup(
      <AiSettings config={baseConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    // Assert Tailwind classes match the app theme
    expect(html).toContain('class="input"');
    expect(html).toContain('class="label"');
    expect(html).toContain('class="btn-primary"');
    expect(html).toContain('class="btn-ghost"');
  });

  it('labels only the neutral providers', () => {
    expect(Object.keys(PROVIDER_LABELS)).toEqual(['anthropic', 'openai', 'google', 'ollama', 'llamacpp']);
  });
});
