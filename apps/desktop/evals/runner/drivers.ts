import { runChatTurn } from '../../src/main/agent/chat.ts';
import { runHarnessTurn } from '../../src/main/agent/harness/index.ts';
import { TraceRecorder } from '../lib/trace.ts';
import type { AgentDriver } from '../types.ts';
import { oracleDriver } from './oracle.ts';

/**
 * Baseline driver: the app's real chat turn (system prompt, tool set, 12-step cap and message
 * building untouched), with a TraceRecorder in place of the chat UI. A redesigned harness
 * plugs in by implementing AgentDriver and being registered in DRIVERS.
 */
export const chatDriver: AgentDriver = {
  id: 'chat-v0',
  async runTurn(message, history, ctx) {
    const recorder = new TraceRecorder(message);
    const controller = new AbortController();
    const abort = () => controller.abort();
    ctx.signal.addEventListener('abort', abort);
    try {
      await runChatTurn(ctx.chatId, message, history, controller, { emit: recorder.emit, onStep: recorder.onStep });
      return recorder.finish();
    } catch (err) {
      return recorder.finish({ error: err instanceof Error ? err.message : String(err), timedOut: ctx.signal.aborted });
    } finally {
      ctx.signal.removeEventListener('abort', abort);
    }
  },
};

/** Harness v1 (state doc, router, planner, compiler macros, verifier) on the same tasks. */
export const harnessV1Driver: AgentDriver = {
  id: 'harness-v1',
  async runTurn(message, history, ctx) {
    const recorder = new TraceRecorder(message);
    const controller = new AbortController();
    const abort = () => controller.abort();
    ctx.signal.addEventListener('abort', abort);
    try {
      // the stand-in user presses Apply on every confirmation (the trace records how many were shown)
      await runHarnessTurn(ctx.chatId, message, history, controller, { emit: recorder.emit, onStep: recorder.onStep }, { mode: ctx.mode, confirm: async () => 'apply' });
      return recorder.finish();
    } catch (err) {
      return recorder.finish({ error: err instanceof Error ? err.message : String(err), timedOut: ctx.signal.aborted });
    } finally {
      ctx.signal.removeEventListener('abort', abort);
    }
  },
};

/**
 * Does nothing and needs no model. Running the suite with it checks the harness itself
 * (fixtures import and analyse, setups run, checks score an untouched timeline): every task
 * should finish without an infra error and score low.
 */
export const noopDriver: AgentDriver = {
  id: 'noop',
  offline: true,
  async runTurn(message) {
    return new TraceRecorder(message).finish();
  },
};

export const DRIVERS: Record<string, AgentDriver> = {
  [chatDriver.id]: chatDriver,
  [harnessV1Driver.id]: harnessV1Driver,
  [noopDriver.id]: noopDriver,
  [oracleDriver.id]: oracleDriver,
};
