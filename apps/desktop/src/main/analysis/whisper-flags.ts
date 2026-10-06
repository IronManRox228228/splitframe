/**
 * whisper.cpp >= 1.8 enables flash attention by default, and with it the per-token
 * timestamps drift: trailing punctuation tokens stretch across pauses, so a 3s silence
 * between two sentences shows up as one long word and silence removal finds nothing.
 * Disabling it restores accurate word timing. Older builds have no such flag (flash
 * attention was opt-in there), so only pass it when `--help` lists it.
 */
export function timestampAccuracyFlags(helpText: string): string[] {
  return /--no-flash-attn\b/.test(helpText) ? ['-nfa'] : [];
}
