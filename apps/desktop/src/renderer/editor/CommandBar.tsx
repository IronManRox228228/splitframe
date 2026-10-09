import { useEffect, useRef, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';
import { AgentModePill } from './AgentModePill.tsx';

/** Floating "Ask SplitFrame" bar. Ctrl/Cmd+K focuses it; Enter hands the text to the Assistant. */
export function CommandBar() {
  const [text, setText] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);
  const platform = useEditor((s) => s.appInfo?.platform);
  const sendToAssistant = useEditor((s) => s.sendToAssistant);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k')) return;
      const el = document.activeElement as HTMLElement | null;
      const typingElsewhere =
        el && el !== inputRef.current && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.tagName === 'SELECT' || el.isContentEditable);
      if (typingElsewhere) return;
      e.preventDefault();
      inputRef.current?.focus();
      inputRef.current?.select();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  const submit = () => {
    const message = text.trim();
    if (!message) return;
    setText('');
    inputRef.current?.blur();
    sendToAssistant(message);
  };

  return (
    <div className="absolute left-1/2 top-[2px] -translate-x-1/2 z-10 flex items-center gap-2 max-w-[calc(100%-32px)]">
    <label className="glass w-[440px] max-w-[calc(100%-32px)] h-[42px] flex items-center gap-2.5 pl-4 pr-2 rounded-full text-fg-muted focus-within:border-accent/60">
      <Icon name="sparkles" size={16} className="text-accent" />
      <input
        ref={inputRef}
        value={text}
        onChange={(e) => setText(e.target.value)}
        onKeyDown={(e) => {
          if (e.key === 'Enter' && !e.nativeEvent.isComposing && e.keyCode !== 229) {
            e.preventDefault();
            submit();
          }
          if (e.key === 'Escape') inputRef.current?.blur();
        }}
        placeholder="Ask SplitFrame to edit, like &quot;remove the ums&quot;"
        aria-label="Ask SplitFrame"
        className="flex-1 min-w-0 bg-transparent outline-none text-[13px] text-fg placeholder:text-fg-faint"
      />
      <span className="kbd shrink-0">{platform === 'darwin' ? '⌘ K' : 'Ctrl K'}</span>
    </label>
    <div className="glass rounded-full h-[42px] px-1.5 flex items-center">
      <AgentModePill placement="down" compact />
    </div>
    </div>
  );
}
