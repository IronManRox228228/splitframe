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

describe('AiSettings UI component (Milestone H1)', () => {
  const baseConfig: AiConfig = {
    agentProvider: 'anthropic',
    agentModel: 'claude-3-5-haiku-latest',
    asrModel: 'base.en',
    vlmProvider: 'none',
    llamacppUrl: 'http://127.0.0.1:8080',
    ollamaUrl: 'http://127.0.0.1:11434',
    meterlessUrl: 'https://api.meterless.ai/v1',
  };

  it('includes Meterless in the provider dropdown list', () => {
    const html = renderToStaticMarkup(
      <AiSettings config={baseConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    expect(html).toContain('value="meterless">Meterless (serverless)</option>');
    expect(html).toContain('value="anthropic"');
    expect(html).toContain('Anthropic (cloud)</option>');
    expect(html).toContain('value="openai">OpenAI (cloud)</option>');
    expect(html).toContain('value="google">Google (cloud)</option>');
    expect(html).toContain('value="ollama">Ollama (local)</option>');
    expect(html).toContain('value="llamacpp">llama.cpp server (local)</option>');
  });

  it('renders Meterless endpoint URL and model placeholder when meterless is selected', () => {
    const meterlessConfig: AiConfig = {
      ...baseConfig,
      agentProvider: 'meterless',
      agentModel: '',
    };

    const html = renderToStaticMarkup(
      <AiSettings config={meterlessConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    // Verify Meterless endpoint field is rendered
    expect(html).toContain('Endpoint URL');
    expect(html).toContain('placeholder="https://api.meterless.ai/v1"');
    expect(html).toContain('Meterless OpenAI-compatible serverless inference endpoint');

    // Verify model input has standard placeholder without provider-specific overrides
    expect(html).toContain('placeholder="e.g. claude-sonnet-4-5"');

    // Verify API key field is shown with encrypted storage label
    expect(html).toContain('API key (stored encrypted)');
  });

  it('does not render Meterless endpoint URL when another provider is selected', () => {
    const ollamaConfig: AiConfig = {
      ...baseConfig,
      agentProvider: 'ollama',
    };

    const html = renderToStaticMarkup(
      <AiSettings config={ollamaConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    expect(html).not.toContain('placeholder="https://api.meterless.ai/v1"');
    expect(html).toContain('placeholder="http://127.0.0.1:11434"');
  });

  it('preserves existing UI styling classes and structure', () => {
    const meterlessConfig: AiConfig = {
      ...baseConfig,
      agentProvider: 'meterless',
    };

    const html = renderToStaticMarkup(
      <AiSettings config={meterlessConfig} onSaved={vi.fn()} onClose={vi.fn()} />
    );

    // Assert Tailwind classes match the app theme
    expect(html).toContain('class="input"');
    expect(html).toContain('class="label"');
    expect(html).toContain('class="btn-primary"');
    expect(html).toContain('class="btn-ghost"');
  });

  describe('Federated Catalog (Zetaris) section', () => {
    it('renders Zetaris catalog header and controls', () => {
      const html = renderToStaticMarkup(
        <AiSettings config={baseConfig} onSaved={vi.fn()} onClose={vi.fn()} />
      );

      expect(html).toContain('Federated Catalog (Zetaris)');
      expect(html).toContain('Federation Endpoint');
      expect(html).toContain('Catalog Name');
      expect(html).toContain('placeholder="zts_live_…"');
      expect(html).toContain('value="https://api.zetaris.com/v1/federation/query"');
      expect(html).toContain('value="production_media_catalog"');
    });
  });
});
