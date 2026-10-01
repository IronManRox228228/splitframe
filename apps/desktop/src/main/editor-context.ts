/**
 * The renderer owns selection/playhead/highlighted-range UI state; the main process
 * caches the latest values so tools can resolve "the selected clip" (main prompt §1.4).
 */
export interface EditorContext {
  selection: string[];
  playheadFrame: number;
  highlightedRange?: { startFrame: number; endFrame: number } | null;
  openProjectId?: string;
}

let current: EditorContext = { selection: [], playheadFrame: 0, highlightedRange: null };

export const editorContextCache = {
  get(): EditorContext {
    return current;
  },
  set(ctx: Partial<EditorContext>): void {
    current = { ...current, ...ctx };
  },
  clear(): void {
    current = { selection: [], playheadFrame: 0, highlightedRange: null };
  },
};
