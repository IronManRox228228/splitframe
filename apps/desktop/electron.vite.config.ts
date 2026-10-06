import { defineConfig, externalizeDepsPlugin } from 'electron-vite';
import react from '@vitejs/plugin-react';
import { resolve } from 'node:path';
import { buildCsp } from './src/main/csp.ts';

// The packaged app loads file:// pages, where a response-header CSP is not reliable, so
// production builds carry the policy in the page itself. (The old <meta> tags lacked
// http-equiv and did nothing.) Dev keeps the header set by the main process because it needs
// inline scripts for HMR.
const cspMeta = () => ({
  name: 'cutboard-csp-meta',
  apply: 'build' as const,
  transformIndexHtml(html: string) {
    const tag = `<meta http-equiv="Content-Security-Policy" content="${buildCsp({ dev: false })}" />`;
    return html.replace('<head>', `<head>\n    ${tag}`);
  },
});

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()],
    resolve: {
      alias: {
        '@main': resolve(__dirname, 'src/main'),
      },
    },
    build: {
      lib: {
        entry: resolve(__dirname, 'src/main/index.ts'),
      },
      rollupOptions: {
        external: ['better-sqlite3'],
      },
    },
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    build: {
      lib: {
        entry: resolve(__dirname, 'src/preload/index.ts'),
      },
      // sandboxed preloads must be CommonJS (Electron cannot load ESM preloads)
      rollupOptions: {
        output: {
          format: 'cjs',
          entryFileNames: 'index.cjs',
        },
      },
    },
  },
  renderer: {
    root: resolve(__dirname, 'src/renderer'),
    plugins: [react(), cspMeta()],
    resolve: {
      alias: {
        '@renderer': resolve(__dirname, 'src/renderer'),
      },
    },
    build: {
      rollupOptions: {
        input: {
          index: resolve(__dirname, 'src/renderer/index.html'),
          export: resolve(__dirname, 'src/renderer/export.html')
        },
      },
    },
  },
});
