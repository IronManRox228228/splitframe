import type { Task } from '../types.ts';
import { removePauses, removeFillers, removeRetake } from './speech-cuts.ts';
import { addCaptions, addTitle, vertical, captionsBiggerUndo } from './graphics.ts';
import { trimClip, reorder, deleteSelected, beatCuts } from './clips.ts';
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

export function selectTasks(ids?: string[]): Task[] {
  if (!ids || ids.length === 0) return TASKS;
  const unknown = ids.filter((id) => !TASKS.some((t) => t.id === id));
  if (unknown.length > 0) throw new Error(`Unknown task id(s): ${unknown.join(', ')}. Known: ${TASKS.map((t) => t.id).join(', ')}`);
  return TASKS.filter((t) => ids.includes(t.id));
}
