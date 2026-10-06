export interface CaptionWord {
  w: string;
  startMs: number;
  endMs: number;
}

/**
 * Caption word times are relative to the caption item's own start (so moving, trimming
 * or rippling the item keeps the words in sync), not to the source asset or the timeline.
 */
export function captionClockMs(frame: number, itemStartFrame: number, fps: number): number {
  return Math.round(((frame - itemStartFrame) / fps) * 1000);
}

/**
 * The card of words to show at `timeMs` and which word is active (-1 between words).
 * Returns null before the first word. Between words the card of the most recent word stays up.
 */
export function captionCardAt(
  words: CaptionWord[],
  timeMs: number,
  maxWordsPerCard: number,
): { card: CaptionWord[]; activeIdx: number } | null {
  if (words.length === 0 || timeMs < words[0]!.startMs) return null;
  const activeIdx = words.findIndex((w) => timeMs >= w.startMs && timeMs < w.endMs);
  let anchor = activeIdx;
  if (anchor === -1) {
    for (let i = words.length - 1; i >= 0; i--) {
      if (words[i]!.startMs <= timeMs) {
        anchor = i;
        break;
      }
    }
  }
  const perCard = Math.max(1, maxWordsPerCard);
  const cardStart = Math.floor(anchor / perCard) * perCard;
  const card = words.slice(cardStart, cardStart + perCard);
  return card.length === 0 ? null : { card, activeIdx };
}
