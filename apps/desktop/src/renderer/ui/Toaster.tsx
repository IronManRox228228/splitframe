import { useEditor } from '../store.ts';
import { Icon } from './Icon.tsx';

const DOT = { info: 'bg-fg-faint', success: 'bg-success', error: 'bg-danger' } as const;

export function Toaster() {
  const toasts = useEditor((s) => s.toasts);
  const dismiss = useEditor((s) => s.dismissToast);
  return (
    <div className="fixed bottom-6 left-1/2 -translate-x-1/2 z-50 flex flex-col items-center gap-2 pointer-events-none">
      {toasts.map((t) => (
        <div
          key={t.id}
          role={t.kind === 'error' ? 'alert' : 'status'}
          className="pointer-events-auto flex items-center gap-3 min-h-11 max-w-[min(640px,90vw)] pl-5 pr-1.5 py-1.5 glass-strong rounded-3xl text-sm text-fg"
        >
          <span className={`w-2 h-2 rounded-full shrink-0 ${DOT[t.kind]}`} />
          <span className="flex-1 py-1 select-text break-words">{t.message}</span>
          {t.action && (
            <button
              className="btn-secondary btn-sm"
              onClick={() => {
                t.action!.run();
                dismiss(t.id);
              }}
            >
              {t.action.label}
            </button>
          )}
          <button className="icon-btn-sm" aria-label="Dismiss" onClick={() => dismiss(t.id)}>
            <Icon name="close" size={14} />
          </button>
        </div>
      ))}
    </div>
  );
}
