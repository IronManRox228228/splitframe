import { getSettings, saveSettings, saveSecret, getSecret } from '../settings.ts';
import { importPaths } from '../asset-service.ts';
import { existsSync } from 'node:fs';

export type FederatedStorageType = 'local' | 'studio_nas' | 'cloud_s3' | 'cloud_gcs' | 'unknown';

export interface FederatedAsset {
  id: string;
  uri: string;
  name: string;
  storageType: FederatedStorageType;
  description: string;
  tags: string[];
  transcriptSnippet?: string;
  durationMs: number;
  width: number;
  height: number;
  fps?: number;
  relevanceScore?: number;
  fileSizeBytes?: number;
}

export interface ZetarisQuery {
  naturalLanguage?: string;
  filterTags?: string[];
  storageFilter?: FederatedStorageType[];
  minWidth?: number;
  minHeight?: number;
  limit?: number;
}

export interface ZetarisTelemetry {
  totalQueries: number;
  totalAssetsReturned: number;
  lastLatencyMs: number;
}

export function storageTypeToString(type: FederatedStorageType): string {
  return type;
}

export function storageTypeFromString(s: string): FederatedStorageType {
  const lower = s.toLowerCase().trim();
  if (lower === 'local') return 'local';
  if (lower === 'studio_nas' || lower === 'nas' || lower === 'smb' || lower === 'nfs') return 'studio_nas';
  if (lower === 'cloud_s3' || lower === 's3') return 'cloud_s3';
  if (lower === 'cloud_gcs' || lower === 'gcs') return 'cloud_gcs';
  return 'unknown';
}

export async function getZetarisConfig(): Promise<{ enabled: boolean; endpoint: string; catalogName: string }> {
  const settings = await getSettings();
  return {
    enabled: settings.zetaris?.enabled ?? true,
    endpoint: settings.zetaris?.endpoint ?? 'https://api.zetaris.com/v1/federation/query',
    catalogName: settings.zetaris?.catalogName ?? 'production_media_catalog',
  };
}

export async function setZetarisConfig(patch: { enabled?: boolean; endpoint?: string; catalogName?: string; apiKey?: string }): Promise<void> {
  const settings = await getSettings();
  const currentZetaris = settings.zetaris ?? {};
  const nextZetaris = {
    ...currentZetaris,
    ...(patch.enabled !== undefined ? { enabled: patch.enabled } : {}),
    ...(patch.endpoint !== undefined ? { endpoint: patch.endpoint } : {}),
    ...(patch.catalogName !== undefined ? { catalogName: patch.catalogName } : {}),
  };
  if (patch.apiKey) {
    await saveSecret('zetarisKey', patch.apiKey);
  }
  await saveSettings({ zetaris: nextZetaris });
}

export class ZetarisClient {
  private mockEnabled = false;
  private mockAssets: FederatedAsset[] = [];
  private telemetry: ZetarisTelemetry = {
    totalQueries: 0,
    totalAssetsReturned: 0,
    lastLatencyMs: 0,
  };

  setMockEnabled(enabled: boolean): void {
    this.mockEnabled = enabled;
  }

  isMockEnabled(): boolean {
    return this.mockEnabled;
  }

  setMockAssets(assets: FederatedAsset[]): void {
    this.mockAssets = structuredClone(assets);
  }

  addMockAsset(asset: FederatedAsset): void {
    this.mockAssets.push(structuredClone(asset));
  }

  getTelemetry(): ZetarisTelemetry {
    return { ...this.telemetry };
  }

  resetTelemetry(): void {
    this.telemetry = { totalQueries: 0, totalAssetsReturned: 0, lastLatencyMs: 0 };
  }

  async query(q: ZetarisQuery): Promise<FederatedAsset[]> {
    const t0 = Date.now();
    let results: FederatedAsset[] = [];

    if (this.mockEnabled || this.mockAssets.length > 0) {
      results = this.queryMock(q);
    } else {
      results = await this.queryRemote(q);
    }

    const latency = Date.now() - t0;
    this.telemetry.totalQueries += 1;
    this.telemetry.totalAssetsReturned += results.length;
    this.telemetry.lastLatencyMs = latency;
    return results;
  }

  private queryMock(q: ZetarisQuery): FederatedAsset[] {
    const nlTerms = (q.naturalLanguage ?? '')
      .toLowerCase()
      .split(/\s+/)
      .map((t) => t.trim())
      .filter((t) => t.length > 0);

    const filterTagsLower = (q.filterTags ?? []).map((t) => t.toLowerCase().trim()).filter((t) => t.length > 0);
    const storageFilters = q.storageFilter ?? [];
    const minWidth = q.minWidth ?? 0;
    const minHeight = q.minHeight ?? 0;
    const limit = q.limit && q.limit > 0 ? q.limit : 50;

    const candidates: FederatedAsset[] = [];

    for (const a of this.mockAssets) {
      // Resolution filtering
      if (a.width < minWidth || a.height < minHeight) continue;

      // Storage filter
      if (storageFilters.length > 0 && !storageFilters.includes(a.storageType)) continue;

      // Tags filtering
      if (filterTagsLower.length > 0) {
        const assetTagsLower = (a.tags ?? []).map((t) => t.toLowerCase().trim());
        const hasAllTags = filterTagsLower.every((ft) => assetTagsLower.includes(ft));
        if (!hasAllTags) continue;
      }

      // Relevance scoring
      let score = 0;
      if (nlTerms.length > 0) {
        const nameLower = (a.name ?? '').toLowerCase();
        const descLower = (a.description ?? '').toLowerCase();
        const transLower = (a.transcriptSnippet ?? '').toLowerCase();
        const assetTagsLower = (a.tags ?? []).map((t) => t.toLowerCase().trim());

        let matchedAny = false;
        for (const term of nlTerms) {
          let termMatched = false;
          if (nameLower.includes(term)) {
            score += 3.0;
            termMatched = true;
          }
          if (descLower.includes(term)) {
            score += 2.0;
            termMatched = true;
          }
          if (assetTagsLower.some((tag) => tag.includes(term))) {
            score += 2.5;
            termMatched = true;
          }
          if (transLower.includes(term)) {
            score += 1.5;
            termMatched = true;
          }
          if (termMatched) matchedAny = true;
        }

        if (!matchedAny) continue;
      } else {
        score = 1.0;
      }

      candidates.push({
        ...a,
        relevanceScore: Math.round(score * 10) / 10,
      });
    }

    candidates.sort((a, b) => (b.relevanceScore ?? 0) - (a.relevanceScore ?? 0));
    return candidates.slice(0, limit);
  }

  private async queryRemote(q: ZetarisQuery): Promise<FederatedAsset[]> {
    const config = await getZetarisConfig();
    if (!config.enabled) return [];

    const apiKey = (await getSecret('zetarisKey')) ?? '';
    const res = await fetch(config.endpoint, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${apiKey || 'none'}`,
      },
      body: JSON.stringify({
        catalog: config.catalogName,
        query: q,
      }),
    });

    if (!res.ok) {
      throw new Error(`Zetaris query error ${res.status}: ${await res.text()}`);
    }

    const json = (await res.json()) as { assets?: FederatedAsset[] };
    return json.assets ?? [];
  }
}

export const defaultZetarisClient = new ZetarisClient();

/**
 * Ingest or link a federated asset into the active project.
 * Local or NAS files are linked directly via asset-service.
 */
export async function importFederatedAssetToProject(asset: FederatedAsset): Promise<{ assetId: string; ok: boolean; error?: string }> {
  let localPath = asset.uri;

  if (localPath.startsWith('file://')) {
    localPath = decodeURIComponent(localPath.replace(/^file:\/\/\/?/, ''));
    if (process.platform === 'win32' && !/^[A-Za-z]:/.test(localPath)) {
      // Restore drive letter on Windows if leading slash was present
      localPath = localPath.replace(/^\//, '');
    }
  }

  if (existsSync(localPath)) {
    const summary = await importPaths([localPath]);
    const importedAsset = summary.assets[0] as { id?: string; status?: string } | undefined;
    if (importedAsset?.id && importedAsset.status !== 'failed') {
      return { assetId: importedAsset.id, ok: true };
    }
    const errMsg = summary.failed[0]?.error ?? 'Asset could not be imported by project service.';
    return { assetId: '', ok: false, error: errMsg };
  }

  // Cloud S3 / GCS asset: return asset ID for metadata tracking or placeholder linking
  return { assetId: asset.id, ok: true };
}
