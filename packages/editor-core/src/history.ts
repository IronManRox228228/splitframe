import { Op } from '@cutboard/schema';

export interface UndoGroup {
  /** The ops that produced the change (redo re-applies them). */
  ops: Op[];
  /** Inverses captured at apply time; undo applies them in order. */
  inverses: Op[];
  label?: string;
  actor?: string;
  createdAt?: number;
}

/**
 * Linear undo/redo history over committed op groups. The owner (project service or the
 * renderer store) is responsible for actually applying ops/inverses to the doc — this
 * class only tracks the stack.
 */
export class History {
  private stack: UndoGroup[] = [];
  private index = 0; // number of applied groups; stack[index] is the next redo
  private readonly limit: number;
  private openGroup: { ops: Op[]; inverses: Op[]; label?: string; actor?: string } | null = null;

  constructor(limit = 500) {
    this.limit = limit;
  }

  /** Begin coalescing subsequent pushes into one group (e.g. an agent turn or a drag). */
  beginGroup(label?: string, actor?: string): void {
    if (this.openGroup) this.commitGroup();
    this.openGroup = { ops: [], inverses: [], label, actor };
  }

  /** Record an applied change; if a group is open it joins that group. */
  push(ops: Op[], inverses: Op[], label?: string, actor?: string): void {
    if (ops.length === 0) return;
    if (this.openGroup) {
      this.openGroup.ops.push(...ops);
      // undo applies a group's inverses in order, so later pushes must come first
      this.openGroup.inverses.unshift(...inverses);
    } else {
      this.commit({ ops: [...ops], inverses: [...inverses], label, actor });
    }
  }

  /** Close the open group (if any) as one undoable entry. */
  commitGroup(): UndoGroup | null {
    if (!this.openGroup) return null;
    const group = this.openGroup;
    this.openGroup = null;
    if (group.ops.length === 0) return null;
    return this.commit(group);
  }

  private commit(group: UndoGroup): UndoGroup {
    // discard any redo tail
    this.stack = this.stack.slice(0, this.index);
    this.stack.push({ ...group, createdAt: group.createdAt ?? Date.now() });
    if (this.stack.length > this.limit) this.stack.shift();
    this.index = this.stack.length;
    return group;
  }

  get undoGroup(): UndoGroup | null {
    if (this.openGroup) return this.openGroup;
    return this.index > 0 ? this.stack[this.index - 1]! : null;
  }

  undo(): UndoGroup | null {
    if (this.openGroup) this.commitGroup();
    if (this.index === 0) return null;
    this.index -= 1;
    return this.stack[this.index]!;
  }

  /**
   * Undo one group by running `apply` on it. If `apply` throws (e.g. the doc changed under
   * the history) the position is put back, so the stack and the doc never drift apart.
   */
  undoWith<T>(apply: (group: UndoGroup) => T): T | null {
    const group = this.undo();
    if (!group) return null;
    try {
      return apply(group);
    } catch (err) {
      this.index += 1;
      throw err;
    }
  }

  /** Counterpart of undoWith for redo. */
  redoWith<T>(apply: (group: UndoGroup) => T): T | null {
    const group = this.redo();
    if (!group) return null;
    try {
      return apply(group);
    } catch (err) {
      this.index -= 1;
      throw err;
    }
  }

  redo(): UndoGroup | null {
    if (this.index >= this.stack.length) return null;
    const group = this.stack[this.index]!;
    this.index += 1;
    return group;
  }

  get canUndo(): boolean {
    return this.index > 0;
  }

  get canRedo(): boolean {
    return this.index < this.stack.length;
  }

  /**
   * Forget history that would bring back references to `needle` (an id that no longer exists,
   * e.g. a deleted asset). Undo drops the newest referencing group and everything older, since
   * those groups can no longer be reached without passing through it; redo drops the first
   * referencing group and everything after it. Groups in between stay valid: they apply to the
   * current doc, which never contained the removed thing.
   */
  dropReferences(needle: string): void {
    const refs = (g: UndoGroup) => JSON.stringify(g.ops).includes(needle) || JSON.stringify(g.inverses).includes(needle);
    if (this.openGroup) this.commitGroup();
    let lastUndo = -1;
    for (let i = this.index - 1; i >= 0; i--) {
      if (refs(this.stack[i]!)) {
        lastUndo = i;
        break;
      }
    }
    let firstRedo = this.stack.length;
    for (let i = this.index; i < this.stack.length; i++) {
      if (refs(this.stack[i]!)) {
        firstRedo = i;
        break;
      }
    }
    this.stack = this.stack.slice(lastUndo + 1, firstRedo);
    this.index -= lastUndo + 1;
  }

  clear(): void {
    this.stack = [];
    this.index = 0;
    this.openGroup = null;
  }

  /** For UI labels: "Split clip" etc. */
  get undoLabel(): string | null {
    return this.undoGroup?.label ?? null;
  }
}
