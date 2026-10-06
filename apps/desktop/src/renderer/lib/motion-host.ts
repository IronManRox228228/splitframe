
/**
 * Sandbox host for generated motion-graphic code (main prompt §7): the code runs inside
 * a sandboxed iframe (`sandbox="allow-scripts"`, NO allow-same-origin — an opaque origin
 * with no DOM access to us) that evaluates one frame at a time and posts back scene
 * trees. The iframe loads cbsandbox://motion/index.html from the main process, whose own
 * Content-Security-Policy gives it no network access at all. The renderer window and the
 * hidden export/still windows each get their own host.
 */
export class MotionHost {
  private iframe: HTMLIFrameElement | null = null;
  private ready: Promise<void> | null = null;
  private seq = 0;
  private pending = new Map<number, { resolve: (tree: unknown) => void; reject: (err: Error) => void }>();

  private ensure(): Promise<void> {
    if (this.ready) return this.ready;
    this.ready = new Promise<void>((resolve, reject) => {
      const iframe = document.createElement('iframe');
      iframe.setAttribute('sandbox', 'allow-scripts');
      iframe.style.display = 'none';
      iframe.src = 'cbsandbox://motion/index.html';
      iframe.onload = () => resolve();
      iframe.onerror = () => reject(new Error('sandbox failed to load'));
      document.body.appendChild(iframe);
      this.iframe = iframe;
      window.addEventListener('message', (ev: MessageEvent) => {
        if (ev.source !== iframe.contentWindow) return; // only the sandbox may answer
        const data = ev.data as { seq: number; ok: boolean; tree?: unknown; error?: string };
        if (!data || typeof data.seq !== 'number' || !this.pending.has(data.seq)) return;
        const waiter = this.pending.get(data.seq)!;
        this.pending.delete(data.seq);
        if (data.ok) waiter.resolve(data.tree);
        else waiter.reject(new Error(data.error ?? 'sandbox evaluation failed'));
      });
      // race the load with a timeout
      setTimeout(() => {
        if (this.iframe === iframe) resolve();
      }, 3000);
    });
    return this.ready;
  }

  async evaluate(code: string, props: Record<string, unknown>, frame: number, videoConfig: { width: number; height: number; fps: number; durationInFrames: number }): Promise<unknown> {
    await this.ensure();
    const seq = ++this.seq;
    return new Promise<unknown>((resolve, reject) => {
      const cap = setTimeout(() => {
        this.pending.delete(seq);
        reject(new Error('motion evaluation timed out (infinite loop?)'));
      }, 1500);
      this.pending.set(seq, {
        resolve: (tree) => {
          clearTimeout(cap);
          resolve(tree);
        },
        reject: (err) => {
          clearTimeout(cap);
          reject(err);
        },
      });
      this.iframe!.contentWindow!.postMessage({ seq, code, props, frame, videoConfig }, '*');
    });
  }

  dispose(): void {
    this.iframe?.remove();
    this.iframe = null;
    this.ready = null;
  }
}

declare global {
  interface Window {
    __cutboardMotionRuntime?: string;
  }
}
