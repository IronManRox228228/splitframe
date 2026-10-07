import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';
import { Markdown } from './Markdown.tsx';
import { AiSettings, PROVIDER_LABELS, type AiConfig } from './AiSettings.tsx';

interface ToolCard {
  tool: string;
  /** pairs a result with its call when the same tool runs several times in one turn */
  callId?: string;
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
  note?: string;
  stopped?: boolean;
}

/** Earlier turns as plain text, so the agent remembers the conversation (it only ever sees text). */
function historyOf(turns: ChatTurn[]): { role: 'user' | 'assistant'; content: string }[] {
  return turns
    .map((t) => ({
      role: t.role,
      content: t.text || (t.tools.length > 0 ? `[used tools: ${[...new Set(t.tools.map((c) => c.tool))].join(', ')}]` : ''),
    }))
    .filter((t) => t.content.length > 0);
}

let chatCounter = 0;
const newChatId = () => `chat-${Date.now()}-${chatCounter++}`;

const SUGGESTIONS: { label: string; prompt: string }[] = [
  { label: 'Remove silences', prompt: 'Build a rough cut of my clips: remove pauses over half a second and any repeated takes.' },
  { label: 'Add captions', prompt: 'Add karaoke-style captions to everything I say.' },
  { label: 'Cut to the beat', prompt: 'Cut my video clips to the beat of the music track, one clip per beat.' },
];

/** Turns a raw provider/network error into something a person can act on. */
export function friendlyError(raw: string, config: AiConfig | null): { message: string; connection: boolean } {
  const provider = config?.agentProvider ?? '';
  const url = provider === 'ollama' ? config?.ollamaUrl : config?.llamacppUrl;
  if (/ECONNREFUSED|fetch failed|ECONNRESET|ENOTFOUND|EAI_AGAIN|ETIMEDOUT|network|socket hang up/i.test(raw)) {
    if (provider === 'llamacpp') {
      return { message: `Can't reach the llama.cpp server at ${url || 'http://127.0.0.1:8080'}. Is llama-server running (with --jinja)?`, connection: true };
    }
    if (provider === 'ollama') {
      return { message: `Can't reach Ollama at ${url || 'http://127.0.0.1:11434'}. Is it running?`, connection: true };
    }
    return { message: `Can't reach ${PROVIDER_LABELS[provider] ?? 'the AI provider'}. Check your internet connection and try again.`, connection: true };
  }
  if (/401|403|invalid.*api.?key|incorrect api key|unauthori[sz]ed/i.test(raw)) {
    return { message: 'The provider rejected the API key. Check it in AI settings.', connection: false };
  }
  if (/429|rate.?limit|quota/i.test(raw)) {
    return { message: 'The provider is rate-limiting requests or the quota is used up. Wait a moment and retry.', connection: false };
  }
  return { message: raw, connection: false };
}

export function ChatTab() {
  const doc = useEditor((s) => s.doc);
  const outbox = useEditor((s) => s.assistantOutbox);
  const [turns, setTurns] = useState<ChatTurn[]>([]);
  const [input, setInput] = useState('');
  const [streaming, setStreaming] = useState(false);
  const [showSettings, setShowSettings] = useState(false);
  const [config, setConfig] = useState<AiConfig | null>(null);
  const scrollRef = useRef<HTMLDivElement>(null);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const nearBottomRef = useRef(true);
  const chatIdRef = useRef(newChatId());
  const turnIdxRef = useRef(-1);
  const turnsRef = useRef(turns);
  turnsRef.current = turns;
  const streamingRef = useRef(streaming);
  streamingRef.current = streaming;

  useEffect(() => {
    void window.cutboard.aiGetConfig().then((c) => setConfig(c as AiConfig));
  }, []);

  useEffect(() => {
    const off = window.cutboard.onEvent((envelope) => {
      // ignore stragglers from an earlier (e.g. aborted) chat
      if ((envelope.payload as { chatId?: string } | undefined)?.chatId !== chatIdRef.current) return;
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
          const lastIdx = p.callId ? tools.findIndex((x) => x.callId === p.callId) : tools.map((x) => x.tool).lastIndexOf(p.tool);
          if (p.phase === 'call' || lastIdx === -1) tools.push(p);
          else tools[lastIdx] = p;
          next[turnIdxRef.current] = { ...t, tools };
          return next;
        });
      } else if (envelope.type === 'chat:done') {
        const p = envelope.payload as { chatId: string; error?: string; note?: string; finishReason?: string };
        setTurns((prev) => {
          const next = [...prev];
          const t = next[turnIdxRef.current];
          if (t && (p.error || p.note)) next[turnIdxRef.current] = { ...t, error: p.error, note: p.note };
          return next;
        });
        setStreaming(false);
      }
    });
    return off;
  }, []);

  // follow the stream only while the reader is at (or near) the bottom
  useLayoutEffect(() => {
    const el = scrollRef.current;
    if (el && nearBottomRef.current) el.scrollTo({ top: el.scrollHeight });
  }, [turns]);

  // auto-growing textarea
  useLayoutEffect(() => {
    const ta = textareaRef.current;
    if (!ta) return;
    ta.style.height = 'auto';
    ta.style.height = `${Math.min(ta.scrollHeight, 140)}px`;
  }, [input]);

  const sendFrom = useCallback((message: string, base: ChatTurn[]) => {
    chatIdRef.current = newChatId();
    // the assistant bubble sits after the user bubble appended below
    turnIdxRef.current = base.length + 1;
    nearBottomRef.current = true;
    setTurns([...base, { role: 'user', text: message, tools: [] }, { role: 'assistant', text: '', tools: [] }]);
    setStreaming(true);
    window.cutboard.sendChat(chatIdRef.current, message, historyOf(base));
  }, []);

  const send = (text?: string) => {
    const message = (text ?? input).trim();
    if (!message || streaming) return;
    setInput('');
    sendFrom(message, turnsRef.current);
  };

  // messages typed into the "Ask SplitFrame" command bar
  useEffect(() => {
    if (!outbox) return;
    useEditor.getState().clearAssistantOutbox();
    if (streamingRef.current) {
      setInput(outbox.text);
      useEditor.getState().showToast('The assistant is still replying. Your message is in the box, send it when it finishes.');
      return;
    }
    sendFrom(outbox.text, turnsRef.current);
  }, [outbox, sendFrom]);

  const stop = () => {
    window.cutboard.abortChat(chatIdRef.current);
    setStreaming(false);
    setTurns((prev) => {
      const next = [...prev];
      const t = next[turnIdxRef.current];
      if (t) next[turnIdxRef.current] = { ...t, stopped: true };
      return next;
    });
  };

  const newChat = () => {
    if (streaming) window.cutboard.abortChat(chatIdRef.current);
    chatIdRef.current = newChatId();
    turnIdxRef.current = -1;
    setStreaming(false);
    setTurns([]);
    setInput('');
  };

  const retry = () => {
    const prev = turnsRef.current;
    const lastUser = prev.length >= 2 ? prev[prev.length - 2] : undefined;
    if (!lastUser || lastUser.role !== 'user' || streaming) return;
    sendFrom(lastUser.text, prev.slice(0, -2));
  };

  if (!doc) return null;

  const modelLine = config
    ? `${config.agentModel || 'Default model'} · ${PROVIDER_LABELS[config.agentProvider] ?? config.agentProvider}`
    : 'Loading…';

  return (
    <div className="flex-1 flex flex-col min-h-0">
      <div className="flex items-center justify-between pl-4 pr-3 pt-4 pb-3">
        <div className="flex flex-col gap-0.5 min-w-0">
          <h2 className="heading">Assistant</h2>
          <span className="text-[11px] text-fg-faint truncate" title={modelLine}>
            {modelLine}
          </span>
        </div>
        <div className="flex gap-0.5 shrink-0">
          <button className="icon-btn-sm" aria-label="New chat" title="New chat" onClick={newChat} disabled={turns.length === 0 && !streaming}>
            <Icon name="plus" size={16} />
          </button>
          <button className="icon-btn-sm" aria-label="AI settings" title="AI settings" onClick={() => setShowSettings(true)}>
            <Icon name="settings" size={16} />
          </button>
        </div>
      </div>

      <div
        ref={scrollRef}
        onScroll={(e) => {
          const el = e.currentTarget;
          nearBottomRef.current = el.scrollHeight - el.scrollTop - el.clientHeight < 80;
        }}
        className="flex-1 overflow-y-auto px-4 pb-4 pt-1 flex flex-col gap-3.5 min-h-0"
      >
        {turns.length === 0 && (
          <div className="text-xs text-fg-muted flex flex-col gap-1.5 pt-3">
            <p className="text-fg text-sm font-medium">Ask for an edit</p>
            <p>The assistant sees your footage (transcripts, scenes), the selection and the playhead. Every change is a normal edit, so undo works ({useEditor.getState().appInfo?.platform === 'darwin' ? '⌘Z' : 'Ctrl+Z'}).</p>
          </div>
        )}
        {turns.map((turn, i) => {
          const isLast = i === turns.length - 1;
          return turn.role === 'user' ? (
            <div key={i} className="self-end max-w-[85%] px-3 py-2.5 rounded-[14px_14px_4px_14px] bg-surface-800 text-[13px] leading-[1.45] text-fg whitespace-pre-wrap break-words">
              {turn.text}
            </div>
          ) : (
            <AssistantTurn
              key={i}
              turn={turn}
              live={streaming && i === turnIdxRef.current}
              config={config}
              onRetry={isLast ? retry : undefined}
              onSettings={() => setShowSettings(true)}
            />
          );
        })}
      </div>

      <div className="px-4 pt-3 pb-4 flex flex-col gap-2">
        {!streaming && (
          <div className="flex gap-1.5 flex-wrap">
            {SUGGESTIONS.map((s) => (
              <button key={s.label} className="pill bg-surface-850" onClick={() => send(s.prompt)}>
                {s.label}
              </button>
            ))}
          </div>
        )}
        <div className="flex items-end gap-2 pl-3 pr-2.5 py-2.5 bg-surface-850 border border-surface-700 rounded-[14px] focus-within:border-accent/60">
          <textarea
            ref={textareaRef}
            value={input}
            onChange={(e) => setInput(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing && e.keyCode !== 229) {
                e.preventDefault();
                send();
              }
            }}
            aria-label="Message the assistant"
            placeholder="Ask for an edit…"
            rows={1}
            className="flex-1 bg-transparent outline-none resize-none text-[13px] leading-[20px] text-fg placeholder:text-fg-faint py-[4px] max-h-[140px]"
          />
          {streaming ? (
            <button className="w-7 h-7 rounded-lg bg-surface-700 text-fg flex items-center justify-center shrink-0 hover:bg-surface-600" aria-label="Stop" title="Stop" onClick={stop}>
              <Icon name="stop" size={14} />
            </button>
          ) : (
            <button
              className="w-7 h-7 rounded-lg bg-accent text-surface-950 flex items-center justify-center shrink-0 hover:bg-accent-hover disabled:opacity-40 disabled:pointer-events-none"
              aria-label="Send"
              title="Send (Enter)"
              onClick={() => send()}
              disabled={!input.trim()}
            >
              <Icon name="send" size={14} />
            </button>
          )}
        </div>
      </div>

      {showSettings && config && <AiSettings config={config} onSaved={setConfig} onClose={() => setShowSettings(false)} />}
    </div>
  );
}

function AssistantTurn({
  turn,
  live,
  config,
  onRetry,
  onSettings,
}: {
  turn: ChatTurn;
  live: boolean;
  config: AiConfig | null;
  onRetry?: () => void;
  onSettings: () => void;
}) {
  const err = turn.error ? friendlyError(turn.error, config) : null;
  const empty = !turn.text && turn.tools.length === 0 && !turn.error;
  const copy = () => {
    void navigator.clipboard
      .writeText(turn.text)
      .then(() => useEditor.getState().showToast('Copied.', { kind: 'success' }))
      .catch(() => useEditor.getState().showToast('Could not copy to the clipboard.', { kind: 'error' }));
  };
  return (
    <div className="flex flex-col gap-2 text-[13px] leading-[1.5] text-[#d6d6dc] min-w-0">
      {turn.tools.length > 0 && (
        <div className="flex flex-col gap-1">
          {turn.tools.map((tc, j) => (
            <ToolRow key={j} card={tc} />
          ))}
        </div>
      )}
      {turn.text && <Markdown text={turn.text} />}
      {empty && live && (
        <span className="flex items-center gap-2 text-fg-muted text-xs">
          <span className="w-1.5 h-1.5 rounded-full bg-accent animate-pulse" />
          Thinking…
        </span>
      )}
      {err && (
        <div className="rounded-xl border border-danger/30 bg-danger/10 p-3 flex flex-col gap-2">
          <p className="text-danger text-xs leading-snug">{err.message}</p>
          <div className="flex gap-1.5 flex-wrap">
            {onRetry && (
              <button className="btn-outline btn-sm" onClick={onRetry}>
                <Icon name="retry" size={14} />
                Retry
              </button>
            )}
            <button className="btn-outline btn-sm" onClick={onSettings}>
              <Icon name="settings" size={14} />
              Open settings
            </button>
          </div>
          {err.message !== turn.error && (
            <details className="text-[11px] text-fg-muted">
              <summary className="cursor-pointer select-none">Technical details</summary>
              <pre className="mt-1.5 whitespace-pre-wrap break-words font-mono">{turn.error}</pre>
            </details>
          )}
        </div>
      )}
      {turn.note && <p className="text-xs text-fg-muted">{turn.note}</p>}
      {turn.stopped && (
        <p className="text-[11px] text-fg-muted flex items-center gap-1.5">
          <Icon name="stop" size={11} />
          Stopped
        </p>
      )}
      {!live && turn.text && (
        <div className="flex gap-1.5">
          <button className="btn-outline btn-sm" onClick={copy} title="Copy this reply">
            <Icon name="copy" size={14} />
            Copy
          </button>
        </div>
      )}
    </div>
  );
}

const TOOL_LABELS: Record<string, string> = {
  getTimeline: 'Read timeline',
  listAssets: 'List footage',
  getAsset: 'Read file details',
  getTranscript: 'Read transcript',
  searchTranscript: 'Search transcript',
  captureFrame: 'Look at frame',
  addClip: 'Add clip',
  addText: 'Add text',
  addAudio: 'Add audio',
  updateItem: 'Change clip',
  moveItem: 'Move clip',
  trimItem: 'Trim clip',
  splitItem: 'Split clip',
  cloneItem: 'Duplicate clip',
  deleteItems: 'Delete clips',
  addMarker: 'Add marker',
  addCaptions: 'Add captions',
  removeSilences: 'Remove silences',
  buildRoughCut: 'Build rough cut',
  duckMusic: 'Duck music',
  beatSync: 'Beat sync',
  createMotionGraphic: 'Create motion graphic',
  exportVideo: 'Export',
};

function plural(n: number, one: string, many = `${one}s`): string {
  return `${n} ${n === 1 ? one : many}`;
}

/** One short line describing what a tool did, from whatever its result happens to contain. */
function summarizeResult(result: unknown): string | null {
  if (result == null) return null;
  if (Array.isArray(result)) return plural(result.length, 'result');
  if (typeof result !== 'object') return String(result).slice(0, 80);
  const r = result as Record<string, unknown>;
  for (const key of ['removedCount', 'removed', 'added', 'captionCount', 'cuts', 'created']) {
    if (typeof r[key] === 'number') return `${key.replace(/Count$/, '')}: ${r[key]}`;
  }
  if (Array.isArray(r.assets)) return plural(r.assets.length, 'file');
  if (Array.isArray(r.items) && r.applied) return `Changed ${plural(r.items.length, 'item')}`;
  if (Array.isArray(r.items)) return plural(r.items.length, 'item');
  if (Array.isArray(r.words)) return plural(r.words.length, 'word');
  if (typeof r.durationSeconds === 'number') return `${r.durationSeconds.toFixed(1)} s`;
  if (typeof r.message === 'string') return r.message.slice(0, 100);
  return null;
}

function ToolRow({ card }: { card: ToolCard }) {
  const [open, setOpen] = useState(false);
  const label = TOOL_LABELS[card.tool] ?? card.tool;
  const resultError = typeof (card.result as { error?: unknown } | null)?.error === 'string' ? ((card.result as { error: string }).error) : undefined;
  const failure = card.error ?? resultError;
  const pending = card.phase === 'call';
  const summary = failure ? failure.replace(/\s+/g, ' ').slice(0, 140) : pending ? null : summarizeResult(card.result);
  return (
    <div className="rounded-lg bg-surface-850 border border-surface-800 overflow-hidden">
      <button className="w-full flex items-start gap-2 px-2.5 py-1.5 text-left hover:bg-surface-800 text-xs" onClick={() => setOpen(!open)} aria-expanded={open}>
        <span className={`mt-px shrink-0 ${pending ? 'text-fg-muted animate-pulse' : failure ? 'text-danger' : 'text-success'}`}>
          {pending ? <span className="inline-block w-3.5 h-3.5 rounded-full border-2 border-current border-t-transparent animate-spin" /> : <Icon name={failure ? 'alert' : 'check'} size={14} />}
        </span>
        <span className="flex-1 min-w-0">
          <span className="text-fg-2 font-medium">{label}</span>
          {summary && <span className={`block text-[11px] leading-snug break-words ${failure ? 'text-danger' : 'text-fg-muted'}`}>{summary}</span>}
        </span>
        <Icon name={open ? 'chevronDown' : 'chevronRight'} size={13} className="mt-0.5 text-fg-faint shrink-0" />
      </button>
      {open && (
        <pre className="px-2.5 py-2 border-t border-surface-800 text-[11px] text-fg-muted overflow-x-auto max-h-48 whitespace-pre-wrap font-mono">
          {JSON.stringify(card.phase === 'result' ? (card.result ?? card.error) : card.args, null, 1)}
        </pre>
      )}
    </div>
  );
}
