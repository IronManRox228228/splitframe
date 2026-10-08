import { useEffect, useRef, useState, type ReactNode } from 'react';
import { useEditor, itemName } from '../store.ts';
import { Segmented } from '../ui/Segmented.tsx';

type Align = 'left' | 'center' | 'right';

const SWATCHES: { label: string; value: string }[] = [
  { label: 'White', value: '#ffffff' },
  { label: 'Yellow', value: '#ffd84d' },
  { label: 'Coral', value: '#ff7a59' },
  { label: 'Black', value: '#0a0a0b' },
];

const TYPE_TITLES: Record<string, string> = {
  text: 'Text',
  video: 'Video',
  audio: 'Audio',
  image: 'Image',
  caption: 'Captions',
  shape: 'Shape',
};

/** Right-hand properties panel. Rendered only while exactly one timeline item is selected. */
export function Inspector() {
  const selection = useEditor((s) => s.selection);
  const item = useEditor((s) => (s.selection.length === 1 ? s.doc?.items.find((i) => i.id === s.selection[0]) : undefined));
  if (selection.length !== 1 || !item) return null;

  const patch = async (p: Record<string, unknown>) => {
    try {
      await window.cutboard.callTool('updateItem', { itemId: item.id, patch: p });
    } catch (err) {
      const raw = err instanceof Error ? err.message : String(err);
      useEditor.getState().showToast(raw.replace(/^Error invoking remote method '[^']+': (Error: )?/, '').slice(0, 300), { kind: 'error' });
    }
  };
  const isText = item.type === 'text';
  const isMedia = item.type === 'video' || item.type === 'audio' || item.type === 'image';
  const hasVolume = item.type === 'video' || item.type === 'audio';
  const hasOpacity = item.type !== 'audio';
  const props = item.props as unknown as { text?: string; style?: Record<string, unknown> };
  const style = props.style ?? {};

  const setStyle = (s: Record<string, unknown>) => patch({ props: { ...props, style: { ...style, ...s } } });

  return (
    <aside
      aria-label={`Selected ${TYPE_TITLES[item.type] ?? item.type}`}
      className="relative glass w-[260px] shrink-0 rounded-[22px] flex flex-col gap-5 p-4 overflow-y-auto min-h-0"
    >
      <div className="flex items-center justify-between gap-2">
        <h2 className="heading">{TYPE_TITLES[item.type] ?? item.type}</h2>
        <span className="text-[11px] text-fg-faint">1 selected</span>
      </div>
      <p className="text-xs text-fg-muted truncate -mt-3" title={itemName(item)}>
        {itemName(item)}
      </p>

      {isText && (
        <>
          <Field label="Text">
            <TextContent key={item.id} value={props.text ?? ''} onCommit={(text) => patch({ props: { ...props, text } })} />
          </Field>
          <Field label="Size and alignment">
            <div className="flex gap-2">
              <label className="flex-1 h-9 px-3 rounded-xl bg-[rgba(8,10,9,0.4)] border border-line flex items-center justify-between text-xs text-fg-muted focus-within:border-accent/60">
                Size
                <NumberInput value={Number(style.fontSize ?? 72)} min={8} max={400} ariaLabel="Font size" onCommit={(fontSize) => setStyle({ fontSize })} />
              </label>
            </div>
            <Segmented<Align>
              label="Text alignment"
              value={(style.align as Align) ?? 'center'}
              options={[
                { value: 'left', label: 'Left' },
                { value: 'center', label: 'Center' },
                { value: 'right', label: 'Right' },
              ]}
              onChange={(align) => setStyle({ align })}
            />
          </Field>
          <Field label="Color">
            <div className="flex gap-2 items-center flex-wrap">
              {SWATCHES.map((c) => {
                const active = String(style.color ?? '#ffffff').toLowerCase() === c.value;
                return (
                  <button
                    key={c.value}
                    aria-label={c.label}
                    aria-pressed={active}
                    title={c.label}
                    onClick={() => setStyle({ color: c.value })}
                    className="w-7 h-7 rounded-full border border-line-strong"
                    style={{ background: c.value, boxShadow: active ? '0 0 0 2px #0B0D0C, 0 0 0 3.5px #5FB7A1' : undefined }}
                  />
                );
              })}
              <label className="w-7 h-7 rounded-full border border-dashed border-line-strong text-fg-muted flex items-center justify-center cursor-pointer text-sm hover:text-fg overflow-hidden relative" title="Custom color">
                +
                <input
                  type="color"
                  aria-label="Custom color"
                  value={/^#[0-9a-f]{6}$/i.test(String(style.color)) ? String(style.color) : '#ffffff'}
                  onChange={(e) => void setStyle({ color: e.target.value })}
                  className="absolute inset-0 opacity-0 cursor-pointer"
                />
              </label>
            </div>
          </Field>
        </>
      )}

      {isMedia && (
        <>
          {hasVolume && (
            <Field label="Volume">
              <Slider
                value={item.volume ?? 1}
                min={0}
                max={2}
                step={0.01}
                format={(v) => `${Math.round(v * 100)}%`}
                label="Volume"
                disabled={item.muted}
                onCommit={(volume) => patch({ volume })}
              />
              <label className="flex items-center gap-2 text-xs text-fg-2 cursor-pointer select-none">
                <input type="checkbox" className="accent-accent" checked={Boolean(item.muted)} onChange={(e) => void patch({ muted: e.target.checked })} />
                Muted
              </label>
            </Field>
          )}
          {item.type !== 'image' && (
            <Field label="Speed">
              <Slider
                value={item.speed ?? 1}
                min={0.25}
                max={4}
                step={0.05}
                format={(v) => `${v.toFixed(2).replace(/\.?0+$/, '')}x`}
                label="Speed"
                onCommit={(speed) => patch({ speed })}
              />
            </Field>
          )}
        </>
      )}

      {hasOpacity && (
        <Field label="Opacity">
          <Slider
            value={item.transform?.opacity ?? 1}
            min={0}
            max={1}
            step={0.01}
            format={(v) => `${Math.round(v * 100)}%`}
            label="Opacity"
            onCommit={(opacity) => patch({ transform: { opacity } })}
          />
        </Field>
      )}
    </aside>
  );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="flex flex-col gap-2">
      <span className="text-xs text-fg-muted">{label}</span>
      {children}
    </div>
  );
}

function TextContent({ value, onCommit }: { value: string; onCommit(v: string): void }) {
  const [draft, setDraft] = useState(value);
  useEffect(() => setDraft(value), [value]);
  const commit = () => {
    if (draft.trim() && draft !== value) onCommit(draft);
    else setDraft(value);
  };
  return (
    <textarea
      value={draft}
      aria-label="Text content"
      rows={3}
      maxLength={500}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === 'Escape') {
          setDraft(value);
          (e.target as HTMLTextAreaElement).blur();
        }
      }}
      className="w-full rounded-xl bg-[rgba(8,10,9,0.4)] border border-line px-3 py-2 text-[13px] text-fg outline-none focus:border-accent/60 resize-none"
    />
  );
}

function NumberInput({ value, min, max, ariaLabel, onCommit }: { value: number; min: number; max: number; ariaLabel: string; onCommit(v: number): void }) {
  const [draft, setDraft] = useState(String(value));
  useEffect(() => setDraft(String(value)), [value]);
  const commit = () => {
    const n = Number(draft);
    if (Number.isFinite(n) && n >= min && n <= max && n !== value) onCommit(Math.round(n));
    else setDraft(String(value));
  };
  return (
    <input
      value={draft}
      inputMode="numeric"
      aria-label={ariaLabel}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === 'Enter') (e.target as HTMLInputElement).blur();
        if (e.key === 'Escape') {
          setDraft(String(value));
          (e.target as HTMLInputElement).blur();
        }
      }}
      className="w-14 text-right bg-transparent outline-none font-mono text-xs text-fg"
    />
  );
}

/** Drags update the readout live; the edit is committed once, when the drag ends. */
function Slider({
  value,
  min,
  max,
  step,
  format,
  label,
  disabled,
  onCommit,
}: {
  value: number;
  min: number;
  max: number;
  step: number;
  format(v: number): string;
  label: string;
  disabled?: boolean;
  onCommit(v: number): void;
}) {
  const [draft, setDraft] = useState(value);
  const dirty = useRef(false);
  useEffect(() => {
    setDraft(value);
    dirty.current = false;
  }, [value]);
  const commit = () => {
    if (dirty.current && draft !== value) onCommit(draft);
    dirty.current = false;
  };
  return (
    <div className="flex items-center gap-2.5">
      <input
        type="range"
        aria-label={label}
        min={min}
        max={max}
        step={step}
        value={draft}
        disabled={disabled}
        onChange={(e) => {
          dirty.current = true;
          setDraft(Number(e.target.value));
        }}
        onPointerUp={commit}
        onKeyUp={commit}
        onBlur={commit}
        className="flex-1 accent-accent disabled:opacity-40"
      />
      <span className="font-mono text-xs text-fg-2 w-12 text-right tabular-nums">{format(draft)}</span>
    </div>
  );
}
