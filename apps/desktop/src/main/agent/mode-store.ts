import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { projectService } from '../project-service.ts';
import { DEFAULT_MODE, isAgentMode, type AgentMode } from './harness/modes.ts';

/**
 * The agent mode lives with the project (a small file next to the plan), so a project reopens in
 * the mode it was left in. Only the renderer's own IPC sets it; no agent tool can.
 */

const FILE = 'agent-mode.json';

export function getProjectMode(): AgentMode {
  try {
    if (!projectService.isOpen) return DEFAULT_MODE;
    const path = join(projectService.dir, FILE);
    if (!existsSync(path)) return DEFAULT_MODE;
    const mode = (JSON.parse(readFileSync(path, 'utf8')) as { mode?: unknown }).mode;
    return isAgentMode(mode) ? mode : DEFAULT_MODE;
  } catch {
    return DEFAULT_MODE;
  }
}

export function setProjectMode(mode: AgentMode): void {
  if (!projectService.isOpen) return;
  writeFileSync(join(projectService.dir, FILE), JSON.stringify({ mode }));
}
