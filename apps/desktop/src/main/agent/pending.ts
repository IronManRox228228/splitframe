/**
 * Confirmation cards the harness is waiting on. The harness parks a promise here; the renderer's
 * Apply / Skip comes back over IPC and resolves it. Aborting the turn resolves it as Skip.
 */

type Decision = 'apply' | 'skip';

const waiting = new Map<string, (d: Decision) => void>();

export function waitForDecision(id: string, signal?: AbortSignal): Promise<Decision> {
  return new Promise((resolve) => {
    const done = (d: Decision) => {
      waiting.delete(id);
      signal?.removeEventListener('abort', onAbort);
      resolve(d);
    };
    const onAbort = () => done('skip');
    waiting.set(id, done);
    if (signal?.aborted) onAbort();
    else signal?.addEventListener('abort', onAbort);
  });
}

/** The user's answer. False when nothing was waiting under that id (a stale or invented one). */
export function decide(id: string, decision: Decision): boolean {
  const resolve = waiting.get(id);
  if (!resolve) return false;
  resolve(decision);
  return true;
}
