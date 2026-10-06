import type { z } from 'zod';
import { Actor, Op } from '@cutboard/schema';

/**
 * Tool registry (main prompt §6): every capability is a typed tool with a zod input
 * schema and a structured output, shared by the built-in chat and the MCP server.
 * "One tool registry, two front doors."
 *
 * Milestone 3 fills in the ~50-tool catalog; the plumbing below is the contract.
 */

export interface ToolContext {
  /** Apply ops to the project (main-process project service). */
  applyOps(ops: Op[], actor: Actor, groupLabel?: string): Promise<{ inverses: Op[]; seq: number }>;
  /** Read-only access to doc + assets + transcripts + editor context. */
  getSnapshot(): Promise<unknown>;
  /** Render a frame of the current timeline to a PNG buffer (captureFrame). */
  captureFrame(frame: number, width?: number): Promise<Buffer>;
  /** Export plumbing for the exportVideo/getExportStatus tools. */
  startExport(presetName: string): Promise<unknown>;
  getExportStatus(exportId: string): Promise<unknown>;
  listExportPresets(): Promise<unknown>;
  /** Beat detection for an audio asset (computed + cached on demand). */
  analyzeBeats(assetId: string): Promise<{ bpm: number; beatsMs: number[]; downbeatsMs: number[]; sections: { startMs: number; endMs: number; label: string; energy: number }[] }>;
  /** Static sandbox-policy check for generated motion-graphic code. */
  validateMotion(code: string): { ok: boolean; error?: string };
  /** Reference-style analysis (computed + cached on demand). */
  analyzeReference(assetId: string): Promise<unknown>;
  getReferenceProfile(assetId: string): Promise<unknown>;
  /** Who is calling (builtin-agent | mcp:<client>). */
  actor: Actor;
}

export interface ToolDef<I extends z.ZodType = z.ZodType> {
  name: string;
  /** Written for an LLM: what it does, when to use it, how to recover. */
  description: string;
  input: I;
  /** Mutating tools return the affected items' new state so agents can self-verify. */
  mutates: boolean;
  /** `input` is zod-validated by ToolRegistry.call before the handler runs. */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  handler(input: any, ctx: ToolContext): Promise<unknown>;
}

export class ToolRegistry {
  private tools = new Map<string, ToolDef>();

  register<I extends z.ZodType>(tool: ToolDef<I>): void {
    if (this.tools.has(tool.name)) throw new Error(`Tool ${tool.name} already registered`);
    this.tools.set(tool.name, tool as unknown as ToolDef);
  }

  get(name: string): ToolDef | undefined {
    return this.tools.get(name);
  }

  list(): ToolDef[] {
    return [...this.tools.values()];
  }

  /** For MCP tools/list. */
  describe(): { name: string; description: string; inputSchema: unknown; mutates: boolean }[] {
    return this.list().map((t) => ({
      name: t.name,
      description: t.description,
      inputSchema: zodToJsonSchemaish(t.input),
      mutates: t.mutates,
    }));
  }

  async call(name: string, rawInput: unknown, ctx: ToolContext): Promise<unknown> {
    const tool = this.tools.get(name);
    if (!tool) {
      throw new Error(`Unknown tool ${name}. Available tools: ${[...this.tools.keys()].join(', ')}.`);
    }
    const input = tool.input.parse(rawInput);
    return tool.handler(input, ctx);
  }
}

/** Minimal JSON-schema-ish shape for clients that don't speak zod. */
function zodToJsonSchemaish(schema: z.ZodType): unknown {
  const anySchema = schema as unknown as { _def?: { typeName?: string; shape?: () => Record<string, z.ZodType> } };
  const def = anySchema._def;
  if (def?.shape) {
    const shape = def.shape();
    const props: Record<string, unknown> = {};
    for (const [key, value] of Object.entries(shape)) {
      props[key] = zodToJsonSchemaish(value);
    }
    return { type: 'object', properties: props };
  }
  const typeName = def?.typeName ?? '';
  if (typeName.includes('string')) return { type: 'string' };
  if (typeName.includes('number')) return { type: 'number' };
  if (typeName.includes('boolean')) return { type: 'boolean' };
  if (typeName.includes('array')) return { type: 'array' };
  return {};
}

export const registry = new ToolRegistry();
