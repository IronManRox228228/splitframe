import { z } from 'zod';

/** Prefixed id types — plain strings at runtime, distinct types for safety. */
export type ProjectId = string;
export type AssetId = string;
export type TrackId = string;
export type ItemId = string;
export type MarkerId = string;
export type ExportId = string;
export type JobId = string;

const id = (prefix: string) => z.string().regex(new RegExp(`^${prefix}_[0-9a-f]{10,}$`));

export const projectIdSchema = id('prj');
export const assetIdSchema = id('ast');
export const trackIdSchema = id('trk');
export const itemIdSchema = id('itm');
export const markerIdSchema = id('mrk');
export const exportIdSchema = id('exp');
export const jobIdSchema = id('job');

/**
 * Ids are generated where ops are *created* (tool handlers, UI actions), never inside the
 * apply engine — the op log must be deterministic when replayed.
 */
export function newId(prefix: string): string {
  const rand =
    typeof globalThis.crypto !== 'undefined' && 'randomUUID' in globalThis.crypto
      ? globalThis.crypto.randomUUID().replace(/-/g, '').slice(0, 16)
      : Math.random().toString(16).slice(2, 10).padEnd(16, '0');
  return `${prefix}_${rand}`;
}

export type Actor = 'user' | 'builtin-agent' | `mcp:${string}`;

export const actorSchema = z.string() as z.ZodType<Actor>;
