/** Tiny event bus so any main-process module can broadcast to all windows. */
type BroadcastFn = (channel: string, payload: unknown) => void;
let fn: BroadcastFn = () => undefined;

export function setBroadcast(fn_: BroadcastFn): void {
  fn = fn_;
}

export function broadcast(channel: string, payload: unknown): void {
  fn(channel, payload);
}
