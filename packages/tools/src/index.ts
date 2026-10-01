export * from './registry.ts';
export * from './tools/read.ts';
export * from './tools/edit.ts';
export * from './tools/export-tools.ts';

import { ToolRegistry } from './registry.ts';
import { READ_TOOLS } from './tools/read.ts';
import { EDIT_TOOLS } from './tools/edit.ts';
import { EXPORT_TOOLS } from './tools/export-tools.ts';

/** Register the milestone-3 tool catalog on a fresh registry. */
export function createToolRegistry(): ToolRegistry {
  const registry = new ToolRegistry();
  for (const tool of [...READ_TOOLS, ...EDIT_TOOLS, ...EXPORT_TOOLS]) {
    registry.register(tool as never);
  }
  return registry;
}
