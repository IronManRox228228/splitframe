import { useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';

interface ToolCard {
  tool: string;
  phase: 'call' | 'result';
  args?: unknown;
  result?: unknown;
  error?: string;
}

interface ChatTurn {
  role: 'user' | 'assistant';
  text: string;
  tools: ToolCard[];
  error?: string;
}

let chatCounter = 0;
const newChatId = () => `chat-${Date.now()}-${chatCounter++}`;

export function ChatTab() {
  const doc = useEditor((s) => s.doc);
  const [turns, setTurns] = useState<ChatTurn[]>([]);
  const [input, setInput] = useState('');
  const [streaming, setStreaming] = useState(false);
  const [showConfig, setShowConfig] = useState(false);
  const [config, setConfig] = useState<{ agentProvider: string; agentModel: string; asrModel: string; vlmProvider: string; llamacppUrl: string } | null>(null);
  const [apiKey, setApiKey] = useState('');
  const scrollRef = useRef<HTMLDivElement>(null);
  const chatIdRef = useRef(newChatId());
  const turnIdxRef = useRef(-1);

  useEffect(() => {
    void window.cutboard.aiGetConfig().then((c) => setConfig(c as never));
  }, []);

  useEffect(() => {
    const off = window.cutboard.onEvent((envelope) => {
      if (envelope.type === 'chat:delta') {
        const { text } = envelope.payload as { chatId: string; text: string };
        setTurns((prev) => {
          const next = [...prev];
          const t = next[turnIdxRef.current];
          if (t) next[turnIdxRef.current] = { ...t, text: t.text + text };
          return next;
        });
      } else if (envelope.type === 'chat:tool') {
        const p = envelope.payload as ToolCard & { chatId: string };
        setTurns((prev) => {
          const next = [...prev];
          const t = next[turnIdxRef.current];
          if (!t) return prev;
          const tools = [...t.tools];
          const lastIdx = tools.map((x) => x.tool).lastIndexOf(p.tool);
          if (p.phase === 'call' || lastIdx === -1) tools.push(p);
          else tools[lastIdx] = p;
          next[turnIdxRef.current] = { ...t, tools };
          return next;
        });
      } else if (envelope.type === 'chat:done') {
        const p = envelope.payload as { chatId: string; error?: string; finishReason?: string };
        setTurns((prev) => {
          const next = [...prev];
          const t = next[turnIdxRef.current];
          if (t && p.error) next[turnIdxRef.current] = { ...t, error: p.error };
          return next;
        });
        setStreaming(false);
      }
    });
    return off;
  }, []);

  useEffect(() => {
    scrollRef.current?.scrollTo({ top: scrollRef.current.scrollHeight });
  }, [turns]);

  const send = () => {
    const message = input.trim();
    if (!message || streaming) return;
    setInput('');
    chatIdRef.current = newChatId();
    turnIdxRef.current = turns.length;
    setTurns((prev) => [...prev, { role: 'user', text: message, tools: [] }, { role: 'assistant', text: '', tools: [] }]);
    setStreaming(true);
    window.cutboard.sendChat(chatIdRef.current, message);
  };

  if (!doc) return null;

  return (
    <div className="flex-1 flex flex-col min-h-0">
      <div ref={scrollRef} className="flex-1 overflow-y-auto p-3 space-y-3">
        {turns.length === 0 && (
          <div className="text-xs text-neutral-500 space-y-2 pt-4">
            <p className="text-neutral-300 font-medium">Ask the agent to edit your timeline.</p>
            <p>It sees your footage (transcripts, scenes), the selection, and the playhead.</p>
            <div className="space-y-1.5 pt-2">
              <Suggestion label="Build a rough cut and remove silences" onClick={() => setInput('Build a rough cut of my clips: remove pauses over half a second and any repeated takes.')} />
              <Suggestion label="Add karaoke captions" onClick={() => setInput('Add karaoke-style captions to everything I say.')} />
              <Suggestion label="Cut to the beat" onClick={() => setInput('Cut my video clips to the beat of the music track, one clip per beat.')} />
            </div>
          </div>
        )}
        {turns.map((turn, i) => (
          <div key={i} className={turn.role === 'user' ? 'flex justify-end' : ''}>
            <div
              className={`max-w-[92%] rounded-lg px-3 py-2 text-xs leading-relaxed whitespace-pre-wrap ${
                turn.role === 'user' ? 'bg-accent/15 text-accent' : 'bg-surface-800 text-neutral-200'
              }`}
            >
              {turn.text || (turn.tools.length === 0 ? (streaming && i === turnIdxRef.current ? '…' : '') : '')}
              {turn.tools.map((tc, j) => (
                <ToolCallCard key={j} card={tc} />
              ))}
              {turn.error && <p className="mt-2 text-red-400">{turn.error}</p>}
            </div>
          </div>
        ))}
      </div>

      <div className="p-3 border-t border-line space-y-2">
        <div className="flex gap-2">
          <textarea
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                send();
              }
            }}
            placeholder="Ask anything… (Enter to send)"
            rows={2}
            className="flex-1 bg-surface-800 border border-line rounded px-2.5 py-2 text-xs outline-none focus:border-accent-dim resize-none"
          />
          {streaming ? (
            <button className="btn-outline self-end" onClick={() => window.cutboard.abortChat(chatIdRef.current)}>
              Stop
            </button>
          ) : (
            <button className="btn-primary self-end" onClick={send} disabled={!input.trim()}>
              Send
            </button>
          )}
        </div>
        <div className="flex items-center justify-between text-[10px] text-neutral-600">
          <button className="hover:text-neutral-400" onClick={() => setShowConfig(!showConfig)}>
            ⚙ AI settings {config ? `· ${config.agentProvider}${config.agentModel ? `/${config.agentModel}` : ''}` : ''}
          </button>
          <span>Every edit is a real op — undo works on the agent too (⌘Z).</span>
        </div>
        {showConfig && config && (
          <div className="panel p-3 space-y-2 text-xs">
            <label className="block">
              <span className="text-neutral-500">Provider</span>
              <select
                value={config.agentProvider}
                onChange={(e) => setConfig({ ...config, agentProvider: e.target.value })}
                className="w-full mt-1 bg-surface-800 border border-line rounded px-2 py-1.5 outline-none"
              >
                <option value="anthropic">Anthropic (cloud)</option>
                <option value="openai">OpenAI (cloud)</option>
                <option value="google">Google (cloud)</option>
                <option value="ollama">Ollama (local)</option>
                <option value="llamacpp">llama.cpp server (local)</option>
              </select>
            </label>
            <label className="block">
              <span className="text-neutral-500">Model (optional override)</span>
              <input
                value={config.agentModel}
                onChange={(e) => setConfig({ ...config, agentModel: e.target.value })}
                placeholder={config.agentProvider === 'llamacpp' ? 'blank = whatever llama-server loaded' : 'e.g. claude-sonnet-4-5'}
                className="w-full mt-1 bg-surface-800 border border-line rounded px-2 py-1.5 outline-none"
              />
            </label>
            {config.agentProvider === 'llamacpp' && (
              <label className="block">
                <span className="text-neutral-500">Server URL</span>
                <input
                  value={config.llamacppUrl}
                  onChange={(e) => setConfig({ ...config, llamacppUrl: e.target.value })}
                  placeholder="http://127.0.0.1:8080"
                  className="w-full mt-1 bg-surface-800 border border-line rounded px-2 py-1.5 outline-none"
                />
                <span className="block mt-1 text-[10px] text-neutral-600">
                  Start llama-server with <code>--jinja</code> or the agent can't call editing tools.
                </span>
              </label>
            )}
            {config.agentProvider !== 'ollama' && (
              <label className="block">
                <span className="text-neutral-500">
                  {config.agentProvider === 'llamacpp' ? 'API key (only if started with --api-key)' : 'API key (stored encrypted)'}
                </span>
                <input
                  value={apiKey}
                  onChange={(e) => setApiKey(e.target.value)}
                  type="password"
                  placeholder="sk-…"
                  className="w-full mt-1 bg-surface-800 border border-line rounded px-2 py-1.5 outline-none"
                />
              </label>
            )}
            <button
              className="btn-primary w-full"
              onClick={() => {
                void window.cutboard
                  .aiSetConfig({
                    agentProvider: config.agentProvider,
                    agentModel: config.agentModel,
                    ...(config.agentProvider === 'llamacpp' && config.llamacppUrl.trim() ? { llamacppUrl: config.llamacppUrl.trim() } : {}),
                    ...(apiKey && config.agentProvider === 'llamacpp' ? { llamacppKey: apiKey } : {}),
                    ...(apiKey && config.agentProvider !== 'llamacpp' && config.agentProvider !== 'ollama' ? { agentKey: apiKey } : {}),
                  })
                  .then(() => {
                    setApiKey('');
                    setShowConfig(false);
                    useEditor.getState().showToast('AI settings saved.');
                  })
                  .catch(() => useEditor.getState().showToast('Could not save AI settings — check the server URL.'));
              }}
            >
              Save
            </button>
          </div>
        )}
      </div>
    </div>
  );
}

function Suggestion({ label, onClick }: { label: string; onClick: () => void }) {
  return (
    <button onClick={onClick} className="block w-full text-left rounded border border-line px-2 py-1.5 hover:border-accent-dim hover:text-neutral-300 transition-colors">
      {label}
    </button>
  );
}

const TOOL_LABELS: Record<string, string> = {
  getTimeline: 'Read timeline',
  listAssets: 'List footage',
  getTranscript: 'Read transcript',
  searchTranscript: 'Search transcript',
  captureFrame: 'Look at frame',
  addClip: 'Add clip',
  addCaptions: 'Add captions',
  removeSilences: 'Remove silences',
  buildRoughCut: 'Build rough cut',
  duckMusic: 'Duck music',
  beatSync: 'Beat sync',
  createMotionGraphic: 'Create motion graphic',
  exportVideo: 'Export',
};

function ToolCallCard({ card }: { card: ToolCard }) {
  const [open, setOpen] = useState(false);
  const label = TOOL_LABELS[card.tool] ?? card.tool;
  return (
    <div className="mt-2 rounded border border-line bg-surface-900 overflow-hidden">
      <button className="w-full flex items-center gap-1.5 px-2 py-1 text-[10px] text-left hover:bg-surface-800" onClick={() => setOpen(!open)}>
        <span className={card.phase === 'call' ? 'text-amber-400 animate-pulse' : 'text-emerald-400'}>{card.phase === 'call' ? '◌' : '✓'}</span>
        <span className="text-neutral-300 font-medium">{label}</span>
        {card.error && <span className="text-red-400">failed</span>}
        <span className="flex-1" />
        <span className="text-neutral-600">{open ? '−' : '+'}</span>
      </button>
      {open && (
        <pre className="px-2 py-1.5 text-[10px] text-neutral-500 overflow-x-auto max-h-40 whitespace-pre-wrap">
          {JSON.stringify(card.phase === 'result' ? (card.result ?? card.error) : card.args, null, 1)}
        </pre>
      )}
    </div>
  );
}

