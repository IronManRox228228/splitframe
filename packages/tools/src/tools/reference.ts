import { z } from 'zod';
import type { ToolDef, ToolContext } from '../registry.ts';

/**
 * Reference style (main prompt §1.2, milestone 8): analyze one reference video's cut
 * rhythm + coarse grade, then copy the chosen properties onto the project. The reference
 * footage itself NEVER enters the output.
 */

interface Snapshot {
  doc: {
    project: { fps: number; referenceAssetId?: string; width: number; height: number };
    tracks: { id: string; kind: string }[];
    items: { id: string; type: string; trackId: string }[];
  };
}

function requireSnapshot(snap: unknown): Snapshot {
  const s = snap as Snapshot;
  if (!s || typeof s !== 'object' || !('doc' in s)) throw new Error('No project open.');
  return s;
}

export const setReferenceAsset: ToolDef = {
  name: 'setReferenceAsset',
  description:
    'Designate one imported video as the project reference (it appears under the References concept; it is never used in the output). Then call analyzeReferenceStyle.',
  input: z.object({ assetId: z.string().nullable() }),
  mutates: true,
  async handler(input, ctx) {
    await ctx.applyOps([{ type: 'project.setReference', assetId: input.assetId }], ctx.actor, 'setReferenceAsset');
    return { applied: true, referenceAssetId: input.assetId };
  },
};

export const analyzeReferenceStyle: ToolDef = {
  name: 'analyzeReferenceStyle',
  description:
    'Analyze the reference video: average shot length, cuts per minute, coarse color stats (luma/saturation averages). Cached per asset. Read the profile with describeReference, then apply what the user wants with applyReferenceStyle.',
  input: z.object({ assetId: z.string().optional().describe('Default = the project reference asset') }),
  mutates: false,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const assetId = input.assetId ?? snapshot.doc.project.referenceAssetId;
    if (!assetId) throw new Error('No reference asset set. Use setReferenceAsset first.');
    return ctx.analyzeReference(assetId);
  },
};

export const describeReference: ToolDef = {
  name: 'describeReference',
  description: 'Read the analyzed reference style profile (cut rhythm, color stats, notes on what is and is not captured).',
  input: z.object({ assetId: z.string().optional() }),
  mutates: false,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const assetId = input.assetId ?? snapshot.doc.project.referenceAssetId;
    if (!assetId) throw new Error('No reference asset set.');
    const profile = await ctx.getReferenceProfile(assetId);
    if (!profile) throw new Error('Reference not analyzed yet — call analyzeReferenceStyle first.');
    return profile;
  },
};

export const applyReferenceStyle: ToolDef = {
  name: 'applyReferenceStyle',
  description:
    'Copy measured reference properties onto the project. Properties: "grade" (brightness/saturation effects approximating the reference color stats), "pacing" (returns suggested cuts/minute for beatSync/rough-cut targets — pacing is applied by you via those tools). Never copies footage.',
  input: z.object({
    properties: z.array(z.enum(['grade', 'pacing'])).min(1),
    assetId: z.string().optional().describe('Reference asset; default = project reference'),
    itemIds: z.array(z.string()).optional().describe('Video items to grade; default = all video items'),
  }),
  mutates: true,
  async handler(input, ctx) {
    const snapshot = requireSnapshot(await ctx.getSnapshot());
    const assetId = input.assetId ?? snapshot.doc.project.referenceAssetId;
    if (!assetId) throw new Error('No reference asset set.');
    const profile = (await ctx.getReferenceProfile(assetId)) as {
      color: { lumaAvg: number; saturationAvg: number };
      cutsPerMinute: number;
      avgShotLengthSec: number;
    } | null;
    if (!profile) throw new Error('Reference not analyzed yet — call analyzeReferenceStyle first.');
    const result: Record<string, unknown> = {};
    const ops: import('@cutboard/schema').Op[] = [];
    if (input.properties.includes('grade')) {
      // map measured stats to gentle corrections relative to neutral (luma 128, sat ~64)
      const brightness = Math.max(-0.3, Math.min(0.3, (128 - profile.color.lumaAvg) / 255));
      const saturation = Math.max(0.2, Math.min(2.5, profile.color.saturationAvg / 64));
      const videoItems = snapshot.doc.items.filter(
        (i) => i.type === 'video' && (input.itemIds ? input.itemIds.includes(i.id) : true),
      );
      for (const item of videoItems) {
        const effectId = `ref-grade-${item.id.slice(4, 12)}`;
        ops.push({
          type: 'effect.add',
          itemId: item.id,
          effect: { id: effectId, type: 'brightness', params: { amount: Number(brightness.toFixed(3)) } },
        });
        ops.push({
          type: 'effect.add',
          itemId: item.id,
          effect: { id: `${effectId}-sat`, type: 'saturation', params: { amount: Number(saturation.toFixed(3)) } },
        });
      }
      result['gradedItems'] = videoItems.length;
      result['brightness'] = Number(brightness.toFixed(3));
      result['saturation'] = Number(saturation.toFixed(3));
    }
    if (input.properties.includes('pacing')) {
      result['suggestedCutsPerMinute'] = profile.cutsPerMinute;
      result['suggestedAvgShotLengthSec'] = profile.avgShotLengthSec;
      result['note'] = 'Pacing is a target — realize it with buildRoughCut/beatSync density settings.';
    }
    if (ops.length > 0) await ctx.applyOps(ops, ctx.actor, 'applyReferenceStyle: grade');
    return { applied: true, ...result };
  },
};

export const REFERENCE_TOOLS: ToolDef[] = [setReferenceAsset, analyzeReferenceStyle, describeReference, applyReferenceStyle];

export type { ToolContext };
