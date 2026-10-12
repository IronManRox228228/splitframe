import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { z } from 'zod';
import {
  storageTypeToString,
  storageTypeFromString,
  getZetarisConfig,
  setZetarisConfig,
  ZetarisClient,
  importFederatedAssetToProject,
  type FederatedAsset,
  type ZetarisQuery,
} from './analysis/zetaris.ts';

// Mock dependencies for settings and asset-service
vi.mock('./settings.ts', () => {
  let inMemorySettings: Record<string, unknown> = {};
  const inMemorySecrets = new Map<string, string>();

  return {
    getSettings: vi.fn(async () => inMemorySettings),
    saveSettings: vi.fn(async (patch: Record<string, unknown>) => {
      inMemorySettings = { ...inMemorySettings, ...patch };
    }),
    saveSecret: vi.fn(async (slot: string, value: string) => {
      inMemorySecrets.set(slot, value);
    }),
    getSecret: vi.fn(async (slot: string) => inMemorySecrets.get(slot) ?? null),
    __resetState: () => {
      inMemorySettings = {};
      inMemorySecrets.clear();
    },
  };
});

vi.mock('./asset-service.ts', () => ({
  importPaths: vi.fn(async (paths: string[]) => ({
    assets: paths.map((p) => ({ id: `ast_${p.replace(/[^a-zA-Z0-9]/g, '_')}`, status: 'analyzed' })),
    imported: paths.length,
    skipped: [],
    failed: [],
  })),
}));

vi.mock('node:fs', () => ({
  existsSync: vi.fn((path: string) => {
    // Treat existing test paths as real files
    if (typeof path === 'string' && (path.includes('existing_clip.mp4') || path.includes('C:/Media/drone.mov'))) {
      return true;
    }
    return false;
  }),
}));

describe('Zetaris Federated Media Catalog (Milestone H2)', () => {
  describe('Storage Type Conversions', () => {
    it('converts types to string representations', () => {
      expect(storageTypeToString('local')).toBe('local');
      expect(storageTypeToString('studio_nas')).toBe('studio_nas');
      expect(storageTypeToString('cloud_s3')).toBe('cloud_s3');
      expect(storageTypeToString('cloud_gcs')).toBe('cloud_gcs');
      expect(storageTypeToString('unknown')).toBe('unknown');
    });

    it('parses various storage aliases correctly', () => {
      expect(storageTypeFromString('local')).toBe('local');
      expect(storageTypeFromString('LOCAL')).toBe('local');
      expect(storageTypeFromString('studio_nas')).toBe('studio_nas');
      expect(storageTypeFromString('nas')).toBe('studio_nas');
      expect(storageTypeFromString('smb')).toBe('studio_nas');
      expect(storageTypeFromString('nfs')).toBe('studio_nas');
      expect(storageTypeFromString('cloud_s3')).toBe('cloud_s3');
      expect(storageTypeFromString('s3')).toBe('cloud_s3');
      expect(storageTypeFromString('cloud_gcs')).toBe('cloud_gcs');
      expect(storageTypeFromString('gcs')).toBe('cloud_gcs');
      expect(storageTypeFromString('invalid_type')).toBe('unknown');
      expect(storageTypeFromString('')).toBe('unknown');
    });
  });

  describe('Configuration and Key Storage', () => {
    beforeEach(async () => {
      const { __resetState } = (await import('./settings.ts')) as unknown as { __resetState: () => void };
      __resetState();
    });

    it('returns sensible defaults when no settings are persisted', async () => {
      const config = await getZetarisConfig();
      expect(config.enabled).toBe(true);
      expect(config.endpoint).toBe('https://api.zetaris.com/v1/federation/query');
      expect(config.catalogName).toBe('production_media_catalog');
    });

    it('persists and updates configuration patches', async () => {
      await setZetarisConfig({
        enabled: false,
        endpoint: 'https://custom-zetaris.internal/v1/query',
        catalogName: 'studio_vault_2026',
        apiKey: 'zts_live_key_9999',
      });

      const config = await getZetarisConfig();
      expect(config.enabled).toBe(false);
      expect(config.endpoint).toBe('https://custom-zetaris.internal/v1/query');
      expect(config.catalogName).toBe('studio_vault_2026');

      const { getSecret } = await import('./settings.ts');
      const secret = await getSecret('zetarisKey');
      expect(secret).toBe('zts_live_key_9999');
    });
  });

  describe('IPC Validation Schemas', () => {
    const zetarisSetConfigSchema = z.object({
      enabled: z.boolean().optional(),
      endpoint: z.string().url().optional(),
      catalogName: z.string().min(1).max(120).optional(),
      apiKey: z.string().max(400).optional(),
    });

    const zetarisQuerySchema = z.object({
      naturalLanguage: z.string().max(500).optional(),
      filterTags: z.array(z.string().max(50)).max(50).optional(),
      storageFilter: z.array(z.enum(['local', 'studio_nas', 'cloud_s3', 'cloud_gcs', 'unknown'])).optional(),
      minWidth: z.number().int().min(0).max(7680).optional(),
      minHeight: z.number().int().min(0).max(4320).optional(),
      limit: z.number().int().min(1).max(200).optional(),
    });

    const zetarisImportSchema = z.object({
      id: z.string().min(1).max(120),
      uri: z.string().min(1).max(2048),
      name: z.string().min(1).max(256),
      storageType: z.enum(['local', 'studio_nas', 'cloud_s3', 'cloud_gcs', 'unknown']),
      description: z.string().max(4000).optional().default(''),
      tags: z.array(z.string().max(50)).max(50).optional().default([]),
      transcriptSnippet: z.string().max(4000).optional(),
      durationMs: z.number().nonnegative().optional().default(0),
      width: z.number().int().nonnegative().optional().default(1920),
      height: z.number().int().nonnegative().optional().default(1080),
      fps: z.number().positive().optional(),
      relevanceScore: z.number().optional(),
      fileSizeBytes: z.number().nonnegative().optional(),
    });

    it('accepts valid setConfig payloads', () => {
      const valid = zetarisSetConfigSchema.parse({
        enabled: true,
        endpoint: 'https://api.zetaris.com/v1/federation/query',
        catalogName: 'main_catalog',
        apiKey: 'secret_abc123',
      });
      expect(valid.enabled).toBe(true);
      expect(valid.catalogName).toBe('main_catalog');
    });

    it('rejects invalid endpoint URLs in setConfig', () => {
      expect(() =>
        zetarisSetConfigSchema.parse({
          endpoint: 'not-a-valid-url',
        }),
      ).toThrow();
    });

    it('accepts valid query inputs and enforces limits', () => {
      const valid = zetarisQuerySchema.parse({
        naturalLanguage: 'tokyo drone 4k',
        filterTags: ['city', 'night'],
        storageFilter: ['cloud_s3', 'studio_nas'],
        minWidth: 3840,
        minHeight: 2160,
        limit: 50,
      });
      expect(valid.naturalLanguage).toBe('tokyo drone 4k');
      expect(valid.limit).toBe(50);
    });

    it('rejects invalid storage enum in query', () => {
      expect(() =>
        zetarisQuerySchema.parse({
          storageFilter: ['ftp_server' as never],
        }),
      ).toThrow();
    });

    it('validates import asset schema with defaults', () => {
      const valid = zetarisImportSchema.parse({
        id: 'fed-123',
        uri: 's3://media-bucket/footage/shot_01.mp4',
        name: 'shot_01.mp4',
        storageType: 'cloud_s3',
      });
      expect(valid.description).toBe('');
      expect(valid.tags).toEqual([]);
      expect(valid.width).toBe(1920);
      expect(valid.height).toBe(1080);
      expect(valid.durationMs).toBe(0);
    });
  });

  describe('ZetarisClient Mock Engine', () => {
    let client: ZetarisClient;

    const sampleAssets: FederatedAsset[] = [
      {
        id: 'ast-1',
        uri: 'file:///Media/tokyo_sunset_drone.mp4',
        name: 'tokyo_sunset_drone.mp4',
        storageType: 'local',
        description: 'Spectacular aerial drone flyover of Tokyo Tower at sunset with orange sky.',
        tags: ['tokyo', 'sunset', 'drone', 'aerial', '4k'],
        transcriptSnippet: 'And here you can see the sun setting behind the city skyline.',
        durationMs: 15000,
        width: 3840,
        height: 2160,
        fps: 60,
      },
      {
        id: 'ast-2',
        uri: 'smb://studio-nas/broll/tokyo_street_rain.mp4',
        name: 'tokyo_street_rain.mp4',
        storageType: 'studio_nas',
        description: 'Night street photography in Shibuya during heavy rain with neon reflections.',
        tags: ['tokyo', 'street', 'night', 'rain', 'neon'],
        transcriptSnippet: 'The neon signs reflect across the wet asphalt as crowds cross.',
        durationMs: 8000,
        width: 1920,
        height: 1080,
        fps: 24,
      },
      {
        id: 'ast-3',
        uri: 's3://cloud-vault/raw/san_francisco_fog.mp4',
        name: 'san_francisco_fog.mp4',
        storageType: 'cloud_s3',
        description: 'Golden Gate bridge covered in dense morning fog roll.',
        tags: ['san francisco', 'bridge', 'fog', 'morning'],
        durationMs: 22000,
        width: 3840,
        height: 2160,
        fps: 30,
      },
      {
        id: 'ast-4',
        uri: 'gcs://studio-backup/broll/nature_macro_flower.mov',
        name: 'nature_macro_flower.mov',
        storageType: 'cloud_gcs',
        description: 'Macro shot of bee pollinating lavender bloom.',
        tags: ['nature', 'macro', 'bee', 'lavender'],
        durationMs: 5000,
        width: 1920,
        height: 1080,
        fps: 120,
      },
    ];

    beforeEach(() => {
      client = new ZetarisClient();
      client.setMockEnabled(true);
      client.setMockAssets(sampleAssets);
      client.resetTelemetry();
    });

    it('toggles mock mode correctly', () => {
      expect(client.isMockEnabled()).toBe(true);
      client.setMockEnabled(false);
      expect(client.isMockEnabled()).toBe(false);
    });

    it('returns all assets when query is empty', async () => {
      const results = await client.query({});
      expect(results).toHaveLength(4);
      expect(results.every((r) => r.relevanceScore === 1.0)).toBe(true);
    });

    it('accurately ranks results by multi-criteria relevance score', async () => {
      // Query "tokyo sunset" matches ast-1 on name (+3), tags (+2.5), description (+2), and transcript (+1.5)
      // Matches ast-2 on tokyo only (name +3, tags +2.5)
      const results = await client.query({ naturalLanguage: 'tokyo sunset' });
      expect(results.length).toBeGreaterThanOrEqual(2);
      expect(results[0]!.id).toBe('ast-1');
      expect(results[0]!.relevanceScore).toBeGreaterThan(results[1]!.relevanceScore ?? 0);
    });

    it('filters by storage pool', async () => {
      const s3Results = await client.query({ storageFilter: ['cloud_s3'] });
      expect(s3Results).toHaveLength(1);
      expect(s3Results[0]!.id).toBe('ast-3');

      const studioResults = await client.query({ storageFilter: ['local', 'studio_nas'] });
      expect(studioResults).toHaveLength(2);
      expect(studioResults.map((r) => r.storageType).sort()).toEqual(['local', 'studio_nas']);
    });

    it('filters by resolution thresholds (minWidth and minHeight)', async () => {
      const results4k = await client.query({ minWidth: 3840, minHeight: 2160 });
      expect(results4k).toHaveLength(2);
      expect(results4k.map((r) => r.id).sort()).toEqual(['ast-1', 'ast-3']);
    });

    it('filters by tags with AND conjunction', async () => {
      const results = await client.query({ filterTags: ['tokyo', 'neon'] });
      expect(results).toHaveLength(1);
      expect(results[0]!.id).toBe('ast-2');
    });

    it('applies result limits and respects pagination cap', async () => {
      const limited = await client.query({ limit: 2 });
      expect(limited).toHaveLength(2);
    });

    it('excludes assets that do not match natural language query', async () => {
      const results = await client.query({ naturalLanguage: 'snow mountain snowboarder' });
      expect(results).toHaveLength(0);
    });

    it('tracks telemetry for query counts, assets returned, and latency', async () => {
      expect(client.getTelemetry().totalQueries).toBe(0);

      await client.query({ naturalLanguage: 'tokyo' });
      let telemetry = client.getTelemetry();
      expect(telemetry.totalQueries).toBe(1);
      expect(telemetry.totalAssetsReturned).toBe(2);
      expect(telemetry.lastLatencyMs).toBeGreaterThanOrEqual(0);

      await client.query({ storageFilter: ['cloud_gcs'] });
      telemetry = client.getTelemetry();
      expect(telemetry.totalQueries).toBe(2);
      expect(telemetry.totalAssetsReturned).toBe(3);

      client.resetTelemetry();
      expect(client.getTelemetry().totalQueries).toBe(0);
      expect(client.getTelemetry().totalAssetsReturned).toBe(0);
    });

    it('allows dynamically adding mock assets', async () => {
      client.addMockAsset({
        id: 'ast-new',
        uri: 'file:///Media/new_clip.mp4',
        name: 'new_clip.mp4',
        storageType: 'local',
        description: 'New fresh clip',
        tags: ['fresh'],
        durationMs: 3000,
        width: 1920,
        height: 1080,
      });

      const res = await client.query({ naturalLanguage: 'fresh' });
      expect(res).toHaveLength(1);
      expect(res[0]!.id).toBe('ast-new');
    });
  });

  describe('ZetarisClient Remote API Querying', () => {
    const originalFetch = globalThis.fetch;
    let client: ZetarisClient;

    beforeEach(async () => {
      client = new ZetarisClient();
      client.setMockEnabled(false);
      client.setMockAssets([]);

      const { saveSettings, saveSecret } = (await import('./settings.ts')) as unknown as {
        saveSettings: (p: unknown) => Promise<void>;
        saveSecret: (s: string, v: string) => Promise<void>;
      };
      await saveSettings({
        zetaris: {
          enabled: true,
          endpoint: 'https://api.zetaris.com/v1/federation/query',
          catalogName: 'production_media_catalog',
        },
      });
      await saveSecret('zetarisKey', 'test_bearer_token_123');
    });

    afterEach(() => {
      globalThis.fetch = originalFetch;
      vi.restoreAllMocks();
    });

    it('returns empty array without fetching if Zetaris is disabled in settings', async () => {
      const { saveSettings } = (await import('./settings.ts')) as unknown as {
        saveSettings: (p: unknown) => Promise<void>;
      };
      await saveSettings({ zetaris: { enabled: false } });

      const fetchSpy = vi.fn();
      globalThis.fetch = fetchSpy;

      const res = await client.query({ naturalLanguage: 'drone' });
      expect(res).toEqual([]);
      expect(fetchSpy).not.toHaveBeenCalled();
    });

    it('sends correct headers, authorization, and payload to remote endpoint', async () => {
      let requestUrl = '';
      let requestHeaders: Record<string, string> = {};
      let requestBody: Record<string, unknown> = {};

      globalThis.fetch = vi.fn().mockImplementation(async (url: string, init?: RequestInit) => {
        requestUrl = url;
        requestHeaders = init?.headers as Record<string, string>;
        requestBody = JSON.parse(init?.body as string);
        return {
          ok: true,
          status: 200,
          json: async () => ({
            assets: [
              {
                id: 'remote-1',
                uri: 's3://remote-vault/clip.mp4',
                name: 'clip.mp4',
                storageType: 'cloud_s3',
                description: 'Remote clip',
                tags: ['remote'],
                durationMs: 10000,
                width: 3840,
                height: 2160,
              },
            ],
          }),
        } as unknown as Response;
      });

      const res = await client.query({ naturalLanguage: 'cloud drone', limit: 15 });
      expect(res).toHaveLength(1);
      expect(res[0]!.id).toBe('remote-1');
      expect(requestUrl).toBe('https://api.zetaris.com/v1/federation/query');
      expect(requestHeaders['Authorization']).toBe('Bearer test_bearer_token_123');
      expect(requestHeaders['Content-Type']).toBe('application/json');
      expect(requestBody.catalog).toBe('production_media_catalog');
      expect(requestBody.query).toEqual({ naturalLanguage: 'cloud drone', limit: 15 });
    });

    it('handles HTTP 401 Unauthorized error from remote catalog', async () => {
      globalThis.fetch = vi.fn().mockImplementation(async () => {
        return {
          ok: false,
          status: 401,
          text: async () => 'Invalid or expired Zetaris authorization token.',
        } as unknown as Response;
      });

      await expect(client.query({ naturalLanguage: 'test' })).rejects.toThrow(
        'Zetaris query error 401: Invalid or expired Zetaris authorization token.',
      );
    });

    it('handles HTTP 429 Rate Limit error from remote catalog', async () => {
      globalThis.fetch = vi.fn().mockImplementation(async () => {
        return {
          ok: false,
          status: 429,
          text: async () => 'Rate limit exceeded: max 60 queries/min.',
        } as unknown as Response;
      });

      await expect(client.query({ naturalLanguage: 'test' })).rejects.toThrow(
        'Zetaris query error 429: Rate limit exceeded: max 60 queries/min.',
      );
    });
  });

  describe('importFederatedAssetToProject', () => {
    it('imports local file when it exists on filesystem', async () => {
      const asset: FederatedAsset = {
        id: 'fed-loc-1',
        uri: 'file:///C:/Media/drone.mov',
        name: 'drone.mov',
        storageType: 'local',
        description: 'Drone footage',
        tags: ['drone'],
        durationMs: 10000,
        width: 3840,
        height: 2160,
      };

      const result = await importFederatedAssetToProject(asset);
      expect(result.ok).toBe(true);
      expect(result.assetId).toContain('ast_');
    });

    it('returns federated asset id directly for cloud storage assets', async () => {
      const asset: FederatedAsset = {
        id: 'fed-s3-999',
        uri: 's3://studio-bucket/raw/tokyo_sunset.mp4',
        name: 'tokyo_sunset.mp4',
        storageType: 'cloud_s3',
        description: 'Cloud asset in S3',
        tags: ['s3', 'sunset'],
        durationMs: 14000,
        width: 3840,
        height: 2160,
      };

      const result = await importFederatedAssetToProject(asset);
      expect(result.ok).toBe(true);
      expect(result.assetId).toBe('fed-s3-999');
    });
  });
});
