import { useEffect, useRef, useState, type ReactNode } from 'react';

export interface MenuItem {
  label: string;
  onSelect(): void;
  danger?: boolean;
  disabled?: boolean;
  separatorBefore?: boolean;
}

/** A trigger plus a popover list of actions; closes on outside click, Escape or selection. */
export function Menu({ trigger, items, align = 'end' }: { trigger: (props: { open: boolean; toggle(): void }) => ReactNode; items: MenuItem[]; align?: 'start' | 'end' }) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        setOpen(false);
      }
    };
    window.addEventListener('mousedown', onDown);
    window.addEventListener('keydown', onKey, true);
    ref.current?.querySelector<HTMLElement>('[role="menuitem"]')?.focus();
    return () => {
      window.removeEventListener('mousedown', onDown);
      window.removeEventListener('keydown', onKey, true);
    };
  }, [open]);

  return (
    <div ref={ref} className="relative" onClick={(e) => e.stopPropagation()}>
      {trigger({ open, toggle: () => setOpen((o) => !o) })}
      {open && (
        <div
          role="menu"
          className={`absolute z-30 top-full mt-1.5 min-w-[176px] p-1.5 rounded-xl bg-surface-850 border border-surface-700 shadow-[0_12px_32px_rgba(0,0,0,0.5)] flex flex-col ${align === 'end' ? 'right-0' : 'left-0'}`}
          onKeyDown={(e) => {
            if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp') return;
            e.preventDefault();
            const els = [...e.currentTarget.querySelectorAll<HTMLElement>('[role="menuitem"]:not([disabled])')];
            const i = els.indexOf(document.activeElement as HTMLElement);
            els[(i + (e.key === 'ArrowDown' ? 1 : -1) + els.length) % els.length]?.focus();
          }}
        >
          {items.map((it) => (
            <div key={it.label} className="contents">
              {it.separatorBefore && <div className="h-px bg-surface-700 my-1" />}
              <button
                role="menuitem"
                disabled={it.disabled}
                onClick={() => {
                  setOpen(false);
                  it.onSelect();
                }}
                className={`h-8 px-2.5 rounded-lg text-left text-sm disabled:opacity-40 hover:bg-surface-800 focus:bg-surface-800 outline-none ${it.danger ? 'text-danger' : 'text-fg-2 hover:text-fg'}`}
              >
                {it.label}
              </button>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
