import { useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';

/** Calls an agent tool from the UI; failures become a plain toast. Returns the tool result or null. */
async function runTool(name: string, args: unknown): Promise<unknown | null> {
  try {
    return await window.cutboard.callTool(name, args);
  } catch (err) {
    const raw = err instanceof Error ? err.message : String(err);
    useEditor.getState().showToast(raw.replace(/^Error invoking remote method '[^']+': (Error: )?/, '').slice(0, 300), { kind: 'error' });
    return null;
  }
}

function firstItemId(result: unknown): string | undefined {
  return (result as { items?: { id: string }[] } | null)?.items?.[0]?.id;
}

interface TextPreset {
  label: string;
  hint: string;
  size: number;
  weight: number;
  seconds: number;
  text: string;
  style: Record<string, unknown>;
  x?: number;
  y?: number;
}

const TEXT_PRESETS: TextPreset[] = [
  { label: 'Title', hint: 'Big and centered', size: 22, weight: 600, seconds: 3, text: 'Your title', style: { fontSize: 96, fontWeight: 700, align: 'center' } },
  { label: 'Subtitle', hint: 'Smaller line', size: 16, weight: 500, seconds: 3, text: 'Your subtitle', style: { fontSize: 56, fontWeight: 500, align: 'center' } },
  { label: 'Body', hint: 'Plain text', size: 13, weight: 400, seconds: 4, text: 'Your text', style: { fontSize: 40, fontWeight: 400, align: 'center', strokeWidth: 0 } },
  {
    label: 'Lower third',
    hint: 'Name tag, bottom left',
    size: 13,
    weight: 500,
    seconds: 4,
    text: 'Name · Title',
    x: -420,
    y: 380,
    style: { fontSize: 44, fontWeight: 600, align: 'left', strokeWidth: 0, backgroundColor: 'rgba(10,10,11,0.7)' },
  },
];

function PanelShell({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="flex-1 flex flex-col min-h-0 gap-3.5 p-4 overflow-y-auto">
      <h2 className="heading">{title}</h2>
      {children}
    </div>
  );
}

const PRESET_CARD =
  'h-[84px] rounded-2xl bg-[rgba(8,10,9,0.4)] border border-line text-fg flex flex-col items-center justify-center gap-1 hover:border-line-strong transition-colors disabled:opacity-40 disabled:pointer-events-none';

export function TextPanel() {
  const doc = useEditor((s) => s.doc);
  const select = useEditor((s) => s.select);

  const add = async (p: TextPreset) => {
    const state = useEditor.getState();
    if (!state.doc) return;
    const fps = state.doc.project.fps;
    const res = await runTool('addText', {
      text: p.text,
      startFrame: Math.round(state.playhead),
      durationFrames: Math.round(p.seconds * fps),
      style: p.style,
      ...(p.x !== undefined ? { x: p.x } : {}),
      ...(p.y !== undefined ? { y: p.y } : {}),
    });
    const id = firstItemId(res);
    if (id) select(id);
  };

  return (
    <PanelShell title="Text">
      <p className="text-xs text-fg-muted -mt-2">Adds text at the playhead. Edit it in the panel on the right.</p>
      <div className="grid grid-cols-2 gap-2.5">
        {TEXT_PRESETS.map((p) => (
          <button key={p.label} className={PRESET_CARD} disabled={!doc} onClick={() => void add(p)} title={p.hint}>
            <span style={{ fontSize: p.size, fontWeight: p.weight }}>{p.label}</span>
          </button>
        ))}
      </div>
    </PanelShell>
  );
}

const CAPTION_PRESETS: { label: string; preset?: 'karaoke' | 'bold' | 'serif'; size: number; weight: number; hint: string }[] = [
  { label: 'Karaoke', preset: 'karaoke', size: 15, weight: 600, hint: 'Highlights each word as it is spoken' },
  { label: 'Bold pop', preset: 'bold', size: 15, weight: 600, hint: 'Big uppercase captions' },
  { label: 'Serif', preset: 'serif', size: 15, weight: 500, hint: 'Serif font with an outline' },
  { label: 'Boxed', size: 13, weight: 500, hint: 'Coming soon' },
];

export function CaptionsPanel() {
  const [busy, setBusy] = useState<string | null>(null);
  const hasSpeech = useEditor((s) => s.assets.some((a) => a.hasSpeech));

  const generate = async (preset: 'karaoke' | 'bold' | 'serif') => {
    setBusy(preset);
    const res = await runTool('addCaptions', { preset });
    setBusy(null);
    if (res) useEditor.getState().showToast('Captions added from the transcript.', { kind: 'success' });
  };

  return (
    <PanelShell title="Captions">
      <p className="text-xs text-fg-muted -mt-2">
        {hasSpeech
          ? 'Generates captions from the speech in your clips.'
          : 'Captions come from the transcript, so import a clip with speech first.'}
      </p>
      <div className="grid grid-cols-2 gap-2.5">
        {CAPTION_PRESETS.map((p) => (
          <button
            key={p.label}
            className={PRESET_CARD}
            disabled={!p.preset || busy !== null}
            title={p.hint}
            onClick={() => p.preset && void generate(p.preset)}
          >
            <span style={{ fontSize: p.size, fontWeight: p.weight }}>{busy === p.preset ? 'Working…' : p.label}</span>
            {!p.preset && <span className="text-[11px] text-fg-muted">Coming soon</span>}
          </button>
        ))}
      </div>
    </PanelShell>
  );
}

export function AudioPanel() {
  const assets = useEditor((s) => s.assets);
  const doc = useEditor((s) => s.doc);
  const selection = useEditor((s) => s.selection);
  const importMedia = useEditor((s) => s.importMedia);
  const addAssetToTimeline = useEditor((s) => s.addAssetToTimeline);

  const audio = assets.filter((a) => a.kind === 'audio');
  const selectedItem = selection.length === 1 ? doc?.items.find((i) => i.id === selection[0]) : undefined;
  const canDuck = selectedItem?.type === 'audio';

  const duck = async () => {
    if (!selectedItem) return;
    const res = await runTool('duckMusic', { musicItemId: selectedItem.id });
    if (res) useEditor.getState().showToast('Music will dip under speech.', { kind: 'success' });
  };

  return (
    <PanelShell title="Audio">
      <div className="flex flex-col gap-1.5">
        <span className="label">Your audio</span>
        {audio.length === 0 ? (
          <div className="rounded-2xl border border-dashed border-line-strong p-4 text-center text-xs text-fg-muted flex flex-col items-center gap-2.5">
            No audio files yet
            <button className="btn-outline btn-sm" onClick={() => void importMedia()}>
              <Icon name="upload" size={14} />
              Import audio
            </button>
          </div>
        ) : (
          audio.map((a) => (
            <button
              key={a.id}
              className="flex items-center gap-2.5 h-11 px-3 rounded-xl bg-[rgba(8,10,9,0.4)] border border-line hover:border-line-strong text-left"
              title="Add to the timeline at the playhead"
              onClick={() => void addAssetToTimeline(a.id, Math.round(useEditor.getState().playhead))}
            >
              <Icon name="audio" size={16} className="text-fg-muted shrink-0" />
              <span className="flex-1 min-w-0 text-xs text-fg-2 truncate">{a.originalName}</span>
              <span className="font-mono text-[11px] text-fg-muted">{Math.floor(a.durationMs / 60000)}:{String(Math.round(a.durationMs / 1000) % 60).padStart(2, '0')}</span>
            </button>
          ))
        )}
      </div>
      <div className="flex flex-col gap-1.5">
        <span className="label">Tools</span>
        <div className="grid grid-cols-2 gap-2.5">
          <button
            className={PRESET_CARD}
            disabled={!canDuck}
            title={canDuck ? 'Lower the music while someone is speaking' : 'Select a music clip on the timeline first'}
            onClick={() => void duck()}
          >
            <span className="text-[13px] font-medium">Duck music</span>
          </button>
          <button className={PRESET_CARD} disabled title="Coming soon">
            <span className="text-[13px] font-medium">Fade in/out</span>
            <span className="text-[11px] text-fg-muted">Coming soon</span>
          </button>
          <button className={PRESET_CARD} disabled title="Coming soon">
            <span className="text-[13px] font-medium">Voice-over</span>
            <span className="text-[11px] text-fg-muted">Coming soon</span>
          </button>
        </div>
      </div>
    </PanelShell>
  );
}
