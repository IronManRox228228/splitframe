import { useMemo, useState } from 'react';
import { useEditor } from '../store.ts';
import { Icon } from '../ui/Icon.tsx';
import { Dialog } from '../ui/Dialog.tsx';
import { ProjectCard } from '../home/ProjectCard.tsx';
import { shortFfmpegVersion } from '../home/format.ts';
import logoUrl from '../assets/brand/splitframe.svg';

interface FormatCard {
  key: string;
  title: string;
  hint: string;
  width: number;
  height: number;
  /** size of the little frame outline, matching the aspect ratio */
  box: { w: number; h: number };
}

const FORMATS: FormatCard[] = [
  { key: 'short', title: 'Short', hint: '9:16 · Reels, TikTok', width: 1080, height: 1920, box: { w: 28, h: 50 } },
  { key: 'video', title: 'Video', hint: '16:9 · YouTube', width: 1920, height: 1080, box: { w: 72, h: 40 } },
  { key: 'square', title: 'Square', hint: '1:1 · Feed posts', width: 1080, height: 1080, box: { w: 48, h: 48 } },
  { key: 'portrait', title: 'Portrait', hint: '4:5 · Instagram', width: 1080, height: 1350, box: { w: 40, h: 50 } },
];

/** Strip Electron's "Error invoking remote method ..." wrapper so toasts read as plain sentences. */
const plainError = (err: unknown): string =>
  err instanceof Error ? err.message.replace(/^Error invoking remote method '[^']+': (Error: )?/, '') : String(err);

export function ProjectListScreen() {
  const recents = useEditor((s) => s.recentProjects);
  const createProject = useEditor((s) => s.createProject);
  const openProject = useEditor((s) => s.openProject);
  const refreshRecents = useEditor((s) => s.refreshRecents);
  const showToast = useEditor((s) => s.showToast);
  const appInfo = useEditor((s) => s.appInfo);

  const [query, setQuery] = useState('');
  const [renamingId, setRenamingId] = useState<string | null>(null);
  const [deleting, setDeleting] = useState<{ id: string; name: string } | null>(null);
  const [busy, setBusy] = useState(false);

  const shown = useMemo(() => {
    const q = query.trim().toLowerCase();
    return q ? recents.filter((p) => p.name.toLowerCase().includes(q)) : recents;
  }, [recents, query]);

  const fail = (err: unknown) => showToast(plainError(err), { kind: 'error' });

  const create = async (size?: { width: number; height: number }, thenImport = false) => {
    if (busy) return;
    setBusy(true);
    try {
      await createProject(undefined, size);
      if (thenImport) await useEditor.getState().importMedia();
    } catch (err) {
      fail(err);
    } finally {
      setBusy(false);
    }
  };

  const duplicate = async (id: string) => {
    try {
      const copy = await window.cutboard.duplicateProject(id);
      await refreshRecents();
      showToast(`Created "${copy.name}"`, { kind: 'success' });
    } catch (err) {
      fail(err);
    }
  };

  const rename = async (id: string, name: string) => {
    setRenamingId(null);
    try {
      await window.cutboard.renameProject(id, name);
      await refreshRecents();
    } catch (err) {
      fail(err);
    }
  };

  const confirmDelete = async () => {
    if (!deleting) return;
    const { id, name } = deleting;
    setDeleting(null);
    try {
      await window.cutboard.deleteProject(id);
      await refreshRecents();
      showToast(`Deleted "${name}"`);
    } catch (err) {
      fail(err);
    }
  };

  const ffmpeg = appInfo?.ffmpeg;

  const newVideo = () => void create({ width: 1920, height: 1080 });

  return (
    <div className="flex-1 min-h-0 flex flex-col bg-surface-950">
      <main className="flex-1 min-h-0 overflow-y-auto">
        <div className="w-[1120px] max-w-full mx-auto px-8 pt-16 pb-8 flex flex-col items-center gap-11">
          <h1 className="sr-only">SplitFrame</h1>
          <div className="relative flex flex-col items-center gap-[22px]">
            {/* the only gradient in the app: a soft grey glow behind the wordmark */}
            <div
              aria-hidden
              className="absolute left-1/2 top-1/2 w-[1100px] h-[560px] rounded-[50%] pointer-events-none"
              style={{
                transform: 'translate(-50%, -55%)',
                background: 'radial-gradient(closest-side, rgba(236,238,236,0.24), rgba(236,238,236,0.10) 40%, rgba(236,238,236,0.03) 70%, transparent)'
              }}
            />
            <img src={logoUrl} alt="" draggable={false} className="relative w-[340px] h-auto" />
            <p className="relative text-[15px] text-fg-muted text-center">Tell it what the cut should feel like. Your footage never leaves this machine.</p>
          </div>

          <label className="glass relative w-[560px] max-w-full h-[50px] rounded-full flex items-center gap-3 pl-5 pr-2 text-fg-muted focus-within:border-accent/60">
            <Icon name="search" size={17} />
            <input
              value={query}
              onChange={(e) => setQuery(e.target.value)}
              placeholder="Search projects"
              aria-label="Search projects"
              className="flex-1 min-w-0 bg-transparent outline-none text-sm text-fg placeholder:text-fg-faint"
            />
            {query && (
              <button type="button" aria-label="Clear search" className="text-fg-muted hover:text-fg" onClick={() => setQuery('')}>
                <Icon name="close" size={14} />
              </button>
            )}
            <button type="button" disabled={busy} onClick={newVideo} className="btn-primary h-9 px-[18px] text-[13px]">
              New video
            </button>
          </label>

          <section className="w-full" aria-label="Make a video">
            <div className="grid grid-cols-5 gap-3.5">
              {FORMATS.map((f) => (
                <button
                  key={f.key}
                  type="button"
                  disabled={busy}
                  onClick={() => void create({ width: f.width, height: f.height })}
                  className="relative glass h-[132px] rounded-[22px] flex flex-col items-center justify-center gap-3 text-fg hover:border-line-strong hover:bg-white/[0.07] transition-colors disabled:opacity-60"
                >
                  <span className="rounded-md border-[1.5px] border-fg/55" style={{ width: f.box.w, height: f.box.h }} />
                  <span className="flex flex-col gap-0.5">
                    <span className="text-[13px] font-medium">{f.title}</span>
                    <span className="text-[11px] text-fg-muted">{f.hint}</span>
                  </span>
                </button>
              ))}
              <button
                type="button"
                disabled={busy}
                onClick={() => void create(undefined, true)}
                className="h-[132px] rounded-[22px] border border-dashed border-line-strong bg-[rgba(8,10,9,0.35)] hover:border-fg-muted hover:text-fg flex flex-col items-center justify-center gap-3 text-fg-2 transition-colors disabled:opacity-60"
              >
                <Icon name="upload" size={22} strokeWidth={1.6} />
                <span className="flex flex-col gap-0.5">
                  <span className="text-[13px] font-medium">Start from footage</span>
                  <span className="text-[11px] text-fg-muted">Choose video, audio or photos</span>
                </span>
              </button>
            </div>
          </section>

          <section className="w-full flex flex-col gap-3.5">
            <div className="flex items-center gap-3.5">
              <h2 className="heading">Recent</h2>
              <div className="flex-1 h-px bg-line" aria-hidden />
            </div>
            {recents.length === 0 ? (
              <div className="rounded-[22px] border border-dashed border-line-strong py-14 flex flex-col items-center gap-2 text-center">
                <p className="text-sm font-medium text-fg">No videos yet</p>
                <p className="text-xs text-fg-muted">Pick a format above, or start from footage you already have.</p>
              </div>
            ) : shown.length === 0 ? (
              <p className="text-sm text-fg-muted py-6">No projects match &ldquo;{query.trim()}&rdquo;.</p>
            ) : (
              <div className="grid grid-cols-4 gap-4 pb-16">
                {shown.map((p) => (
                  <ProjectCard
                    key={p.id}
                    project={p}
                    renaming={renamingId === p.id}
                    onOpen={() => void openProject(p.id).catch(fail)}
                    onStartRename={() => setRenamingId(p.id)}
                    onCommitRename={(name) => void rename(p.id, name)}
                    onCancelRename={() => setRenamingId(null)}
                    onDuplicate={() => void duplicate(p.id)}
                    onReveal={() => void window.cutboard.revealProjectById(p.id).catch(fail)}
                    onDelete={() => setDeleting({ id: p.id, name: p.name })}
                  />
                ))}
              </div>
            )}
          </section>
        </div>
      </main>

      <footer className="h-10 shrink-0 px-8 flex items-center justify-between text-xs text-fg-faint">
        <span>SplitFrame {appInfo ? `v${appInfo.version}` : ''}</span>
        <span className={ffmpeg || !appInfo ? '' : 'text-danger'}>
          {!appInfo ? '' : ffmpeg ? `Video engine: ffmpeg ${shortFfmpegVersion(ffmpeg.version)}` : "Video engine not found — exports won't work"}
        </span>
      </footer>

      {deleting && (
        <Dialog title="Delete this project?" width={440} onClose={() => setDeleting(null)}>
          <div className="px-6 pt-3 pb-6 flex flex-col gap-5">
            <p className="text-sm text-fg-2">
              &ldquo;{deleting.name}&rdquo; and its project folder (previews, analysis and any exports kept inside it) will be removed from this computer. Your original footage files are not touched.
            </p>
            <div className="flex justify-end gap-2">
              <button className="btn-outline" onClick={() => setDeleting(null)}>
                Cancel
              </button>
              <button className="btn-danger" onClick={() => void confirmDelete()}>
                Delete project
              </button>
            </div>
          </div>
        </Dialog>
      )}
    </div>
  );
}
