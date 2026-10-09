import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vitest/config';

// Separate from the repo's vitest.config.ts so `pnpm test` never regenerates fixtures.
// Run from the repo root: pnpm exec vitest run --config native/tests/fixtures/gen/vitest.config.ts
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../../../..');

export default defineConfig({
  root: repoRoot,
  test: {
    include: ['native/tests/fixtures/gen/*.test.ts'],
    environment: 'node',
  },
});
