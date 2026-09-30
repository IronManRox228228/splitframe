/**
 * Platform abstractions (addendum §1): keep Electron behind thin interfaces so a Tauri
 * or web shell stays possible. The Electron implementation lives in apps/desktop; this
 * package holds the contracts. Expanded as milestones need them.
 */

export interface PlatformFs {
  /** Show an open dialog and return chosen file paths. */
  pickFiles(opts: { title: string; filters?: { name: string; extensions: string[] }[]; multi?: boolean }): Promise<string[]>;
  revealInFileManager(path: string): Promise<void>;
  exists(path: string): Promise<boolean>;
}

export interface PlatformDialogs {
  message(opts: { title: string; body: string }): Promise<void>;
}

export interface PlatformProcesses {
  /** Spawn a sidecar binary with lifecycle supervision (crash + restart policy). */
  spawnSidecar(name: string, args: string[]): { pid: number; kill(): void; onExit(cb: (code: number | null) => void): void };
}

export interface PlatformShell {
  fs: PlatformFs;
  dialogs: PlatformDialogs;
  processes: PlatformProcesses;
}
