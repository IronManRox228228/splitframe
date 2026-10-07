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
            <p className="text-xs text-fg-faint">
              Local-first AI video editor — your footage never leaves this machine.
            </p>
          </div>
        </div>

        <div className="panel p-5">
          <h2 className="text-sm font-medium text-fg-2 mb-3">New project</h2>
          <div className="flex gap-2">
            <input
              value={name}
              onChange={(e) => setName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && name.trim()) void createProject(name.trim());
              }}
              placeholder="Project name"
              className="flex-1 bg-surface-800 border border-line rounded px-3 h-8 text-sm text-fg outline-none focus:border-accent-dim"
            />
            <button className="btn-primary" onClick={() => void createProject(name.trim() || 'Untitled project')}>
              Create
            </button>
          </div>
        </div>

        <div className="mt-5">
          <h2 className="text-sm font-medium text-fg-muted mb-2">Recent projects</h2>
          {recents.length === 0 ? (
            <p className="text-xs text-fg-faint px-1 py-4">
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
                    <span className="text-sm text-fg-2">{p.name}</span>
                    <span className="text-xs text-fg-faint">
                      {new Date(p.updatedAt).toLocaleString()}
                    </span>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>

        <p className="mt-8 text-[11px] text-fg-faint">
          Cutboard v{appInfo?.version ?? '0.1.0'} · {appInfo?.platform ?? ''} · ffmpeg:{' '}
          {appInfo?.ffmpeg ? `${appInfo.ffmpeg.version} (${appInfo.ffmpeg.source}, ${appInfo.ffmpeg.h264Encoder})` : 'not found — run pnpm fetch:ffmpeg'}
        </p>
      </div>
    </div>
  );
}

function Logo() {
  // sliced-play mark — mirrors scripts/generate-logo.mjs playHalves()
  const s = 40;
  const c = s / 2;
  const scale = s * 0.62;
  const gap = s * 0.018;
  const S = (x: number, y: number): [number, number] => [c + (x - 0.5) * scale, c + (y - 0.5) * scale];
  const n = [0.377, 0.926];
  const off = (pts: [number, number][], sign: number) =>
    pts.map(([x, y]) => [x + n[0]! * gap * sign, y + n[1]! * gap * sign] as [number, number]);
  const A = [0.25, 0.15] as const, B = [0.25, 0.85] as const, C = [0.78, 0.5] as const;
  const Q1 = [0.25, 0.581] as const, Q2 = [0.654, 0.417] as const;
  const poly = (pts: [number, number][]) => pts.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(' ');
  const top = poly(off([S(...A), S(...Q2), S(...Q1)], -0.5));
  const bottom = poly(off([S(...Q1), S(...B), S(...C), S(...Q2)], 0.5));
  return (
    <div className="w-10 h-10 rounded-lg bg-accent flex items-center justify-center shadow-lg shadow-accent/10 overflow-hidden">
      <svg width={s} height={s} viewBox={`0 0 ${s} ${s}`}>
        <polygon points={top} fill="#0b0b0d" />
        <polygon points={bottom} fill="#0b0b0d" />
      </svg>
    </div>
  );
}
