import { describe, expect, it } from 'vitest';
import { MAX_HISTORY_CHARS, MAX_HISTORY_TURNS, buildMessages, formatEditorContext } from './messages.ts';

describe('buildMessages', () => {
  it('sends previous turns before the new message', () => {
    const msgs = buildMessages(
      [
        { role: 'user', content: 'add captions' },
        { role: 'assistant', content: 'Done - 12 cards.' },
      ],
      'make them bigger',
    );
    expect(msgs.map((m) => m.role)).toEqual(['user', 'assistant', 'user']);
    expect(msgs[2]!.content).toBe('make them bigger');
  });

  it('drops empty turns and leading assistant turns', () => {
    const msgs = buildMessages(
      [
        { role: 'assistant', content: 'stale' },
        { role: 'user', content: 'hi' },
        { role: 'assistant', content: '   ' },
      ],
      'again',
    );
    // the empty reply disappears, so the two user turns merge instead of breaking alternation
    expect(msgs).toEqual([{ role: 'user', content: 'hi\n\nagain' }]);
  });

  it('keeps the budget by dropping the oldest turns first', () => {
    const big = 'x'.repeat(MAX_HISTORY_CHARS / 2 + 1);
    const msgs = buildMessages(
      [
        { role: 'user', content: 'oldest' },
        { role: 'assistant', content: big },
        { role: 'user', content: 'recent question' },
        { role: 'assistant', content: big },
      ],
      'now',
    );
    expect(msgs.some((m) => m.content === 'oldest')).toBe(false);
    expect(msgs[msgs.length - 1]!.content).toBe('now');
    expect(msgs[0]!.role).toBe('user');
  });

  it('caps the number of turns', () => {
    const history = Array.from({ length: MAX_HISTORY_TURNS * 3 }, (_, i) => ({ role: i % 2 === 0 ? ('user' as const) : ('assistant' as const), content: `t${i}` }));
    expect(buildMessages(history, 'go').length).toBeLessThanOrEqual(MAX_HISTORY_TURNS + 1);
  });

  it('appends the editor context to the new message only', () => {
    const ctx = formatEditorContext({ selection: ['itm_a'], playheadFrame: 90, highlightedRange: null });
    const msgs = buildMessages([{ role: 'user', content: 'q' }, { role: 'assistant', content: 'a' }], 'trim this', ctx);
    expect(msgs[2]!.content).toBe(`trim this\n\n${ctx}`);
    expect(ctx).toBe('<editor_context>selected items: itm_a; playhead: frame 90</editor_context>');
    expect(msgs[0]!.content).toBe('q');
  });
});
