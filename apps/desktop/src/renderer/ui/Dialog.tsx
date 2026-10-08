import { useEffect, useRef, type ReactNode } from 'react';
import { Icon } from './Icon.tsx';

/** Modal dialog: Escape and backdrop close it (unless `locked`), focus moves inside and is kept there. */
export function Dialog({
  title,
  onClose,
  locked = false,
  width = 520,
  children,
}: {
  title: ReactNode;
  onClose(): void;
  locked?: boolean;
  width?: number;
  children: ReactNode;
}) {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const prev = document.activeElement as HTMLElement | null;
    const el = ref.current;
    el?.querySelector<HTMLElement>('[autofocus], input, button:not([aria-label="Close"])')?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !locked) {
        e.stopPropagation();
        onClose();
      }
      if (e.key === 'Tab' && el) {
        const items = [...el.querySelectorAll<HTMLElement>('button, input, select, textarea, a[href], [tabindex]:not([tabindex="-1"])')].filter((n) => !n.hasAttribute('disabled'));
        if (items.length === 0) return;
        const first = items[0]!;
        const last = items[items.length - 1]!;
        if (e.shiftKey && document.activeElement === first) {
          e.preventDefault();
          last.focus();
        } else if (!e.shiftKey && document.activeElement === last) {
          e.preventDefault();
          first.focus();
        }
      }
    };
    window.addEventListener('keydown', onKey, true);
    return () => {
      window.removeEventListener('keydown', onKey, true);
      prev?.focus?.();
    };
  }, [locked, onClose]);

  return (
    <div className="fixed inset-0 z-40 bg-black/60 flex items-center justify-center p-6" onMouseDown={(e) => e.target === e.currentTarget && !locked && onClose()}>
      <div
        ref={ref}
        role="dialog"
        aria-modal="true"
        aria-label={typeof title === 'string' ? title : undefined}
        className="relative glass-strong rounded-[22px] flex flex-col max-h-full overflow-hidden"
        style={{ width }}
      >
        <div className="flex items-center justify-between pl-6 pr-4 pt-5">
          <h2 className="text-[17px] font-semibold text-fg">{title}</h2>
          {!locked && (
            <button className="icon-btn-sm" aria-label="Close" onClick={onClose}>
              <Icon name="close" />
            </button>
          )}
        </div>
        <div className="overflow-y-auto">{children}</div>
      </div>
    </div>
  );
}
