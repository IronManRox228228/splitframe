import { describe, expect, it, vi } from 'vitest';
import type { ToolContext } from '../registry.ts';
import { createToolRegistry } from '../index.ts';
import { searchFederatedMedia, importFederatedAsset, ZETARIS_TOOLS } from './zetaris.ts';

describe('Zetaris Federated Media Tools', () => {
  describe('Tool Registry integration', () => {
    it('registers both Zetaris tools in the default registry', () => {
      const reg = createToolRegistry();
      expect(reg.get('searchFederatedMedia')).toBeDefined();
      expect(reg.get('importFederatedAsset')).toBeDefined();
    });

    it('declares correct mutation flags', () => {
      expect(searchFederatedMedia.mutates).toBe(false);
      expect(importFederatedAsset.mutates).toBe(true);
    });

    it('exports ZETARIS_TOOLS array', () => {
      expect(ZETARIS_TOOLS).toContain(searchFederatedMedia);
      expect(ZETARIS_TOOLS).toContain(importFederatedAsset);
    });
  });

  describe('searchFederatedMedia handler', () => {
    it('calls ctx.searchFederatedMedia with mapped parameters', async () => {
      const mockSearch = vi.fn().mockResolvedValue([
        {
          id: 'fed-1',
          uri: 's3://media/broll/tokyo_sunset.mp4',
          name: 'tokyo_sunset.mp4',
          storageType: 'cloud_s3',
          description: 'Tokyo skyline during golden hour sunset',
          tags: ['sunset', 'tokyo', 'aerial'],
          durationMs: 14000,
          width: 3840,
          height: 2160,
          relevanceScore: 7.5,
        },
      ]);

      const ctx = {
        actor: 'user',
        searchFederatedMedia: mockSearch,
      } as unknown as ToolContext;

      const input = searchFederatedMedia.input.parse({
        query: 'sunset tokyo',
        tags: ['aerial'],
        storage: 'cloud_s3',
        minWidth: 3840,
        limit: 10,
      });

      const res = (await searchFederatedMedia.handler(input, ctx)) as {
        query: string;
        count: number;
        assets: unknown[];
      };

      expect(mockSearch).toHaveBeenCalledWith({
        naturalLanguage: 'sunset tokyo',
        filterTags: ['aerial'],
        storageFilter: ['cloud_s3'],
        minWidth: 3840,
        limit: 10,
      });
      expect(res.query).toBe('sunset tokyo');
      expect(res.count).toBe(1);
      expect(res.assets).toHaveLength(1);
    });

    it('maps storage "all" to undefined filter', async () => {
      const mockSearch = vi.fn().mockResolvedValue([]);
      const ctx = {
        actor: 'user',
        searchFederatedMedia: mockSearch,
      } as unknown as ToolContext;

      const input = searchFederatedMedia.input.parse({
        query: 'nature landscape',
        storage: 'all',
      });

      await searchFederatedMedia.handler(input, ctx);
      expect(mockSearch).toHaveBeenCalledWith({
        naturalLanguage: 'nature landscape',
        filterTags: undefined,
        storageFilter: undefined,
        minWidth: undefined,
        limit: 20,
      });
    });

    it('throws when ctx.searchFederatedMedia is not available', async () => {
      const ctx = {
        actor: 'user',
      } as unknown as ToolContext;

      const input = searchFederatedMedia.input.parse({ query: 'test' });
      await expect(searchFederatedMedia.handler(input, ctx)).rejects.toThrow(
        /Federated catalog search is unavailable/,
      );
    });
  });

  describe('importFederatedAsset handler', () => {
    it('calls ctx.importFederatedAsset with normalized inputs', async () => {
      const mockImport = vi.fn().mockResolvedValue({
        assetId: 'ast_local_123',
        ok: true,
      });

      const ctx = {
        actor: 'user',
        importFederatedAsset: mockImport,
      } as unknown as ToolContext;

      const input = importFederatedAsset.input.parse({
        assetId: 'fed_asset_42',
        uri: 'file:///Volumes/Media/clip1.mov',
        name: 'clip1.mov',
        storageType: 'studio_nas',
        description: 'Drone shot',
        tags: ['drone', '4k'],
      });

      const res = await importFederatedAsset.handler(input, ctx);
      expect(mockImport).toHaveBeenCalledWith({
        assetId: 'fed_asset_42',
        uri: 'file:///Volumes/Media/clip1.mov',
        name: 'clip1.mov',
        storageType: 'studio_nas',
        description: 'Drone shot',
        tags: ['drone', '4k'],
      });
      expect(res).toEqual({ assetId: 'ast_local_123', ok: true });
    });

    it('applies schema defaults for optional fields', async () => {
      const mockImport = vi.fn().mockResolvedValue({ assetId: 'ast_test', ok: true });
      const ctx = {
        actor: 'user',
        importFederatedAsset: mockImport,
      } as unknown as ToolContext;

      const input = importFederatedAsset.input.parse({
        assetId: 'fed_simple',
        uri: 's3://bucket/test.mp4',
        name: 'test.mp4',
      });

      expect(input.storageType).toBe('local');
      expect(input.description).toBe('');
      expect(input.tags).toEqual([]);

      await importFederatedAsset.handler(input, ctx);
      expect(mockImport).toHaveBeenCalledWith({
        assetId: 'fed_simple',
        uri: 's3://bucket/test.mp4',
        name: 'test.mp4',
        storageType: 'local',
        description: '',
        tags: [],
      });
    });

    it('throws when ctx.importFederatedAsset is missing', async () => {
      const ctx = {
        actor: 'user',
      } as unknown as ToolContext;

      const input = importFederatedAsset.input.parse({
        assetId: 'fed_simple',
        uri: 's3://bucket/test.mp4',
        name: 'test.mp4',
      });

      await expect(importFederatedAsset.handler(input, ctx)).rejects.toThrow(
        /Federated asset import is unavailable/,
      );
    });
  });
});
