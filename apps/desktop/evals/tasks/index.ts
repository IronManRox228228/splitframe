import type { Task } from '../types.ts';
import { removePauses, removeFillers, removeRetake } from './speech-cuts.ts';
import { addCaptions, addTitle, vertical, captionsBiggerUndo } from './graphics.ts';
import { trimClip, reorder, deleteSelected, beatCuts } from './clips.ts';
import { HELDOUT } from './heldout.ts';
import { HELDOUT2 } from './heldout2.ts';
import { musicDuck, teaser, exportSd, compound } from './compound.ts';

/** Every eval task, in run order (cheap and fast first, the long ones last). */
export const TASKS: Task[] = [
  addTitle,
  trimClip,
  deleteSelected,
  reorder,
  vertical,
  addCaptions,
  removePauses,
  removeRetake,
  removeFillers,
  musicDuck,
  beatCuts,
  captionsBiggerUndo,
  exportSd,
  teaser,
  compound,
];

export { HELDOUT, HELDOUT2 };

/** `heldout` / `heldout2` select a held-out group (never part of the default run); otherwise ids from any list. */
export function selectTasks(ids?: string[]): Task[] {
  if (!ids || ids.length === 0) return TASKS;
  const all = [...TASKS, ...HELDOUT, ...HELDOUT2];
  const wanted = ids.flatMap((id) => (id === 'heldout' ? HELDOUT.map((t) => t.id) : id === 'heldout2' ? HELDOUT2.map((t) => t.id) : [id]));
  const unknown = wanted.filter((id) => !all.some((t) => t.id === id));
  if (unknown.length > 0) throw new Error(`Unknown task id(s): ${unknown.join(', ')}. Known: ${all.map((t) => t.id).join(', ')}, or the groups "heldout", "heldout2"`);
  return all.filter((t) => wanted.includes(t.id));
}
