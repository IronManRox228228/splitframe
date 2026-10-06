/**
 * Conversation assembly for the built-in agent. Pure (no Electron/AI-SDK imports) so the
 * trimming and role rules can be unit-tested.
 */

export interface ChatTurn {
  role: 'user' | 'assistant';
  content: string;
}

/** Older turns are dropped first once either budget is exceeded. */
export const MAX_HISTORY_TURNS = 30;
export const MAX_HISTORY_CHARS = 24_000;

export interface EditorContextSnapshot {
  selection: string[];
  playheadFrame: number;
  highlightedRange?: { startFrame: number; endFrame: number } | null;
}

/** The "selected clip / playhead" context the system prompt promises is attached to every turn. */
export function formatEditorContext(ctx: EditorContextSnapshot): string {
  const parts = [
    `selected items: ${ctx.selection.length > 0 ? ctx.selection.join(', ') : 'none'}`,
    `playhead: frame ${ctx.playheadFrame}`,
  ];
  if (ctx.highlightedRange) parts.push(`highlighted range: frames ${ctx.highlightedRange.startFrame}-${ctx.highlightedRange.endFrame}`);
  return `<editor_context>${parts.join('; ')}</editor_context>`;
}

/**
 * Previous turns + the new user message as alternating user/assistant messages: empty turns
 * are dropped, the oldest turns are trimmed to the budget, the list starts with a user turn
 * and consecutive same-role turns (e.g. a question whose reply failed) are merged.
 */
export function buildMessages(history: ChatTurn[], message: string, context?: string): ChatTurn[] {
  const kept: ChatTurn[] = [];
  let chars = 0;
  for (let i = history.length - 1; i >= 0 && kept.length < MAX_HISTORY_TURNS; i--) {
    const turn = history[i]!;
    const content = turn.content.trim();
    if (!content) continue;
    chars += content.length;
    if (chars > MAX_HISTORY_CHARS && kept.length > 0) break;
    kept.unshift({ role: turn.role, content });
  }
  while (kept[0]?.role === 'assistant') kept.shift();

  const merged: ChatTurn[] = [];
  for (const turn of kept) {
    const last = merged[merged.length - 1];
    if (last && last.role === turn.role) last.content += `\n\n${turn.content}`;
    else merged.push({ ...turn });
  }

  const finalText = context ? `${message}\n\n${context}` : message;
  const last = merged[merged.length - 1];
  if (last?.role === 'user') last.content += `\n\n${finalText}`;
  else merged.push({ role: 'user', content: finalText });
  return merged;
}
