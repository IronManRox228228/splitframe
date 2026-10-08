export * from './registry.ts';
export * from './tools/read.ts';
export * from './tools/edit.ts';
export * from './tools/export-tools.ts';
export * from './tools/macros.ts';
export * from './tools/project.ts';

import { ToolRegistry } from './registry.ts';
import { READ_TOOLS } from './tools/read.ts';
import { EDIT_TOOLS } from './tools/edit.ts';
import { EXPORT_TOOLS } from './tools/export-tools.ts';
import { MACRO_TOOLS } from './tools/macros.ts';
import { PROJECT_TOOLS } from './tools/project.ts';
import { BEAT_TOOLS } from './tools/beat.ts';
import { MOTION_TOOLS } from './tools/motion.ts';
import { REFERENCE_TOOLS } from './tools/reference.ts';

/** Register the tool catalog on a fresh registry. */
export function createToolRegistry(): ToolRegistry {
  const registry = new ToolRegistry();
  for (const tool of [...READ_TOOLS, ...EDIT_TOOLS, ...MACRO_TOOLS, ...BEAT_TOOLS, ...MOTION_TOOLS, ...REFERENCE_TOOLS, ...EXPORT_TOOLS, ...PROJECT_TOOLS]) {
    registry.register(tool as never);
  }
  return registry;
}
