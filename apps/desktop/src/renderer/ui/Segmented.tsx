export function Segmented<T extends string>({
  value,
  options,
  onChange,
  label,
}: {
  value: T;
  options: readonly { value: T; label: string }[];
  onChange(v: T): void;
  label: string;
}) {
  return (
    <div role="radiogroup" aria-label={label} className="flex p-[3px] rounded-xl bg-[rgba(8,10,9,0.4)] border border-line">
      {options.map((o) => (
        <button
          key={o.value}
          role="radio"
          aria-checked={o.value === value}
          onClick={() => onChange(o.value)}
          className={`flex-1 h-8 rounded-[9px] text-xs transition-colors ${o.value === value ? 'bg-white/[0.12] text-fg font-medium' : 'text-fg-muted hover:text-fg'}`}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}
