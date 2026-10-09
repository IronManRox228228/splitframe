import { memo } from 'react';
import { Icon } from '../ui/Icon.tsx';
import { planProgress, type ConfirmCardData, type PlanCardData, type PlanCardStep } from '../../shared/agent-cards.ts';

/** The assistant's plan card and confirmation card (see src/shared/agent-cards.ts for the data). */

const KIND_LABELS: Record<string, string> = {
  assemble: 'Build the cut',
  remove_pauses: 'Remove pauses',
  remove_fillers: 'Remove filler words',
  remove_retakes: 'Remove repeated takes',
  beat_cut: 'Cut to the beat',
  captions: 'Captions',
  music: 'Music',
  title: 'Title',
  canvas: 'Canvas',
  clip_edit: 'Edit clips',
  export: 'Export',
};

function StepGlyph({ status }: { status: PlanCardStep['status'] }) {
  if (status === 'running') return <span className="inline-block w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />;
  if (status === 'done') return <Icon name="check" size={14} className="text-success" />;
  if (status === 'failed') return <Icon name="alert" size={14} className="text-danger" />;
  if (status === 'skipped') return <Icon name="minus" size={14} className="text-fg-muted" />;
  return <span className="inline-block w-3.5 h-3.5 rounded-full border border-line-strong" />;
}

export const PlanCard = memo(function PlanCard({
  card,
  canAct,
  onRun,
  onEdit,
  onCancel,
}: {
  card: PlanCardData;
  /** no other reply is streaming */
  canAct: boolean;
  onRun(): void;
  onEdit(): void;
  onCancel(): void;
}) {
  const { done, total } = planProgress(card);
  const proposed = card.state === 'proposed';
  return (
    <div className="rounded-xl border border-line bg-[rgba(8,10,9,0.4)] overflow-hidden" data-testid="plan-card" data-state={card.state}>
      <div className="px-3 pt-2.5 pb-2 flex items-start gap-2">
        <Icon name="layers" size={14} className="mt-0.5 text-accent shrink-0" />
        <div className="min-w-0 flex-1">
          <p className="text-[13px] font-medium text-fg leading-snug">{card.summary}</p>
          <p className="text-[11px] text-fg-muted mt-0.5">
            {proposed ? `Plan · ${total} step${total === 1 ? '' : 's'} · nothing has changed yet` : card.state === 'cancelled' ? 'Plan cancelled' : `${done} of ${total} done`}
          </p>
        </div>
      </div>
      <ol className="px-3 pb-2 flex flex-col gap-1.5">
        {card.steps.map((s) => (
          <li key={s.id} className="flex items-start gap-2 text-xs" data-status={s.status}>
            <span className="mt-0.5 shrink-0 w-3.5 h-3.5 flex items-center justify-center">
              <StepGlyph status={s.status} />
            </span>
            <span className="min-w-0 flex-1">
              <span className={s.status === 'pending' ? 'text-fg-2' : 'text-fg'}>{KIND_LABELS[s.kind] ?? s.kind}</span>
              <span className="text-fg-muted"> · {s.goal}</span>
              {(s.note && s.status !== 'pending' && s.status !== 'running' ? s.note : s.preview) && (
                <span className={`block text-[11px] leading-snug break-words ${s.status === 'failed' ? 'text-danger' : 'text-fg-muted'}`}>
                  {s.note && s.status !== 'pending' && s.status !== 'running' ? s.note : s.preview}
                </span>
              )}
            </span>
          </li>
        ))}
      </ol>
      {proposed && (
        <div className="px-3 pb-3 pt-1 flex gap-1.5 flex-wrap">
          <button className="btn-accent btn-sm" onClick={onRun} disabled={!canAct}>
            <Icon name="play" size={12} />
            Run
          </button>
          <button className="btn-outline btn-sm" onClick={onEdit} disabled={!canAct}>
            Edit
          </button>
          <button className="btn-ghost btn-sm" onClick={onCancel}>
            Cancel
          </button>
        </div>
      )}
      {card.state === 'launched' && <p className="px-3 pb-2.5 text-[11px] text-fg-muted">Running below.</p>}
    </div>
  );
});

export const ConfirmCard = memo(function ConfirmCard({ card, onDecide }: { card: ConfirmCardData; onDecide(decision: 'apply' | 'skip'): void }) {
  const pending = card.status === 'pending';
  const guarded = card.reasons.length > 0;
  return (
    <div className={`rounded-xl border bg-[rgba(8,10,9,0.4)] px-3 py-2.5 flex flex-col gap-1.5 ${pending && guarded ? 'border-danger/40' : 'border-line'}`} data-testid="confirm-card" data-status={card.status}>
      <p className="text-[11px] uppercase tracking-wide text-fg-muted">{pending ? (guarded ? 'Needs your OK' : 'Apply this change?') : card.status === 'applied' ? 'Applied' : 'Skipped'}</p>
      <p className="text-[13px] text-fg leading-snug">{card.line}</p>
      {card.counts && <p className="text-[11px] text-fg-muted">{card.counts}</p>}
      {card.reasons.map((r) => (
        <p key={r} className="text-[11px] text-danger leading-snug">
          {r}
        </p>
      ))}
      {pending && (
        <div className="flex gap-1.5 pt-1">
          <button className="btn-accent btn-sm" onClick={() => onDecide('apply')}>
            Apply
          </button>
          <button className="btn-outline btn-sm" onClick={() => onDecide('skip')}>
            Skip
          </button>
        </div>
      )}
    </div>
  );
});
