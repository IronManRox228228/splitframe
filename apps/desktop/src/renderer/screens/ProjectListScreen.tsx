import { useEffect, useState } from 'react';
import { useEditor } from '../store.ts';

export function ProjectListScreen() {
  const recents = useEditor((s) => s.recentProjects);
  const createProject = useEditor((s) => s.createProject);
  const openProject = useEditor((s) => s.openProject);
  const appInfo = useEditor((s) => s.appInfo);
  const [name, setName] = useState('');

  useEffect(() => {
    setName('');
  }, []);

  return (
    <div className="flex-1 flex flex-col items-center justify-center px-6">
      <div className="w-full max-w-2xl">
        <div className="flex items-center gap-3 mb-8">
          <Logo />
          <div>
            <h1 className="text-xl font-semibold text-white tracking-tight">Cutboard</h1>
            <p className="text-xs text-neutral-500">
              Local-first AI video editor — your footage never leaves this machine.
            </p>
          </div>
        </div>

        <div className="panel p-5">
          <h2 className="text-sm font-medium text-neutral-200 mb-3">New project</h2>
          <div className="flex gap-2">
            <input
              value={name}
              onChange={(e) => setName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && name.trim()) void createProject(name.trim());
              }}
              placeholder="Project name"
              className="flex-1 bg-surface-800 border border-line rounded px-3 h-8 text-sm text-neutral-100 outline-none focus:border-accent-dim"
            />
            <button className="btn-primary" onClick={() => void createProject(name.trim() || 'Untitled project')}>
              Create
            </button>
          </div>
        </div>

        <div className="mt-5">
          <h2 className="text-sm font-medium text-neutral-400 mb-2">Recent projects</h2>
          {recents.length === 0 ? (
            <p className="text-xs text-neutral-600 px-1 py-4">
              No projects yet — create one above, then import footage.
            </p>
          ) : (
            <ul className="divide-y divide-line">
              {recents.map((p) => (
                <li key={p.id}>
                  <button
                    onClick={() => void openProject(p.id)}
                    className="w-full flex items-center justify-between px-3 py-2.5 rounded hover:bg-surface-800 text-left transition-colors"
                  >
                    <span className="text-sm text-neutral-200">{p.name}</span>
                    <span className="text-xs text-neutral-600">
                      {new Date(p.updatedAt).toLocaleString()}
                    </span>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>

        <p className="mt-8 text-[11px] text-neutral-700">
          Cutboard v{appInfo?.version ?? '0.1.0'} · {appInfo?.platform ?? ''} · ffmpeg:{' '}
          {appInfo?.ffmpeg ? `${appInfo.ffmpeg.version} (${appInfo.ffmpeg.source}, ${appInfo.ffmpeg.h264Encoder})` : 'not found — run pnpm fetch:ffmpeg'}
        </p>
      </div>
    </div>
  );
}

function Logo() {
  return (
    <div className="w-10 h-10 rounded-lg bg-accent flex items-center justify-center shadow-lg shadow-accent/10">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#0b0b0d" strokeWidth="2.4" strokeLinecap="round">
        <rect x="3" y="5" width="13" height="14" rx="2.5" />
        <path d="M16 9.5 21 7v10l-5-2.5" />
      </svg>
    </div>
  );
}
