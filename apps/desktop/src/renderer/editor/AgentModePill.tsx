import { useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';
import { AGENT_MODE_IDS, type AgentModeId } from '../../shared/agent-cards.ts';

export const MODE_LABELS: Record<AgentModeId, string> = { plan: 'Plan', ask: 'Ask before edits', default: 'Default', auto: 'Auto' };
const MODE_HINTS: Record<AgentModeId, string> = {
  plan: 'Reads and plans only. Nothing changes until you run the plan.',
  ask: 'Shows what each change will do and waits for Apply or Skip.',
  default: 'Small requests apply right away. Big jobs show a plan and wait for Run.',
  auto: 'Runs the whole job without stopping. Still verifies its work.',
};
const DOT: Record<AgentModeId, string> = { plan: 'bg-fg-muted', ask: 'bg-fg-2', default: 'bg-fg', auto: 'bg-accent' };

/** The agent's mode, per project. Click for the list; Shift+Tab in the chat box cycles it. */
export function AgentModePill({ placement, compact }: { placement: 'up' | 'down'; compact?: boolean }) {
  const mode = useEditor((s) => s.agentMode);
  const harness = useEditor((s) => s.agentHarness);
  const setMode = useEditor((s) => s.setAgentMode);
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false);
    };
    window.addEventListener('mousedown', onDown);
    window.addEventListener('keydown', onKey);
    return () => {
      window.removeEventListener('mousedown', onDown);
      window.removeEventListener('keydown', onKey);
    };
  }, [open]);

  const classic = harness === 'classic';
  return (
    <div ref={rootRef} className="relative shrink-0">
      <button
        type="button"
        className={`inline-flex items-center gap-1.5 rounded-full border border-line-strong ${compact ? 'h-7 px-2.5' : 'h-7 px-3'} text-xs text-fg-2 hover:border-fg hover:text-fg transition-colors ${classic ? 'opacity-60' : ''}`}
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={`Assistant mode: ${MODE_LABELS[mode]}`}
        title={classic ? 'The classic assistant does not use modes (switch to the new harness in AI settings)' : `${MODE_HINTS[mode]} (Shift+Tab to change)`}
        onClick={() => setOpen((o) => !o)}
        data-testid="mode-pill"
      >
        <span className={`w-1.5 h-1.5 rounded-full ${DOT[mode]}`} />
        <span className="whitespace-nowrap">{MODE_LABELS[mode]}</span>
        <Icon name={open ? 'chevronDown' : 'chevronRight'} size={11} className="text-fg-faint" />
      </button>
      {open && (
        <div
          role="listbox"
          className={`absolute ${placement === 'up' ? 'bottom-[calc(100%+6px)]' : 'top-[calc(100%+6px)]'} left-0 z-30 glass-strong rounded-2xl p-1.5 w-[260px] flex flex-col`}
        >
          {AGENT_MODE_IDS.map((m) => (
            <button
              key={m}
              role="option"
              aria-selected={m === mode}
              className={`text-left rounded-xl px-3 py-2 flex flex-col gap-0.5 hover:bg-white/[0.07] ${m === mode ? 'bg-white/[0.06]' : ''}`}
              onClick={() => {
                void setMode(m);
                setOpen(false);
              }}
            >
              <span className="flex items-center gap-2 text-[13px] text-fg">
                <span className={`w-1.5 h-1.5 rounded-full ${DOT[m]}`} />
                {MODE_LABELS[m]}
                {m === mode && <Icon name="check" size={12} className="ml-auto text-accent" />}
              </span>
              <span className="text-[11px] leading-snug text-fg-muted pl-3.5">{MODE_HINTS[m]}</span>
            </button>
          ))}
          <p className="px-3 pt-1.5 pb-1 text-[11px] text-fg-faint">Always asks first: removing media, overwriting an export, anything that costs money, or cutting most of the timeline.</p>
        </div>
      )}
    </div>
  );
}
