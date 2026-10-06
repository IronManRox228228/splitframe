import { MOTION_RUNTIME_SOURCE, MOTION_EVAL_SOURCE } from '@cutboard/renderer';

/**
 * The document the motion-graphics iframe loads (cbsandbox://motion/index.html). It is
 * served by the main process with its own restrictive Content-Security-Policy, so unlike
 * an inline `srcdoc` iframe it does not inherit the app's policy and cannot make network
 * requests even if generated code slips past the static identifier check.
 */
export function buildMotionSandboxHtml(
  runtime: string = MOTION_RUNTIME_SOURCE + MOTION_EVAL_SOURCE,
): string {
  const script = runtime.replace(/<\/script/gi, '<\\/script');
  return `<!doctype html><html><head><meta charset="utf-8"></head><body><script>${script}</script><script>
window.addEventListener('message', (ev) => {
  const { seq, code, props, frame, videoConfig } = ev.data;
  try {
    const tree = evaluateFrame(code, props, frame, videoConfig);
    ev.source.postMessage({ seq, ok: true, tree }, '*');
  } catch (err) {
    ev.source.postMessage({ seq, ok: false, error: String((err && err.message) || err) }, '*');
  }
});
</script></body></html>`;
}
