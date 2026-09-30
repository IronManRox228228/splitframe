# openCardboard → **Cutboard**

Local-first, cross-platform desktop AI video editor. You drop in raw footage, the built-in
agent (or any MCP client — Claude Code, Claude Desktop, Codex, Cursor) understands it
(word-level transcript + per-scene visual descriptions + semantic search), and you edit by
describing changes in plain language. The agent edits the **real timeline** through a typed
tool API; a full manual timeline editor is always available for fine-tuning.

**Edits real footage. Never generates video.** Nothing enters the output that you did not
import. Your files stay on your machine unless you explicitly configure a cloud AI provider.

> Product name: **Cutboard** (repo: `openCardboard`). Rename freely — one string in
> `apps/desktop/package.json`.

## Status

Work in progress, milestone by milestone (see `docs/DECISIONS.md` and the desktop addendum):

1. [x] Shell + core — Electron app, typed IPC, SQLite project service, op-based editor core
       with undo/redo, manual timeline, proxy-based preview, basic ffmpeg export.
2. [ ] Import + ingestion — relink, proxies/thumbnails/waveforms, whisper.cpp ASR, scene
       detection, VLM descriptions, embeddings + hybrid search (sqlite-vec + FTS5).
3. [ ] Tool registry + built-in chat.
4. [ ] Local MCP (loopback Streamable HTTP + token + stdio shim).
5–10. Macros, beat sync, motion graphics, effects/masks/tracking, hardware export, packaging.

## Architecture

```
┌─────────────────────────── Desktop app (one install) ───────────────────────────┐
│ Renderer process (React editor UI, Zustand, Player)                              │
│      ▲ typed IPC (contextBridge, zod-validated)                                  │
│ Main process (Node)                                                              │
│   ├─ Project service      SQLite (better-sqlite3) + op log, undo/redo            │
│   ├─ Tool registry        same typed tools for chat and MCP                      │
│   ├─ Built-in agent       Vercel AI SDK, provider adapters (cloud or Ollama)     │
│   ├─ MCP server           127.0.0.1 Streamable HTTP + stdio shim                 │
│   ├─ Job manager          queue, progress, cancel, resume (SQLite-backed)        │
│   └─ Process supervisor   spawns/monitors ffmpeg + ML sidecars                   │
│ Sidecars: bundled ffmpeg/ffprobe · ML worker (ONNX/whisper.cpp) · render worker  │
└──────────────────────────────────────────────────────────────────────────────────┘
```

Timeline state lives in the **main process** (SQLite + append-only op log). The UI and MCP
clients are just clients: agents can edit with the editor window closed (app can sit in the
tray).

## Repo layout

See the desktop addendum §6. `packages/*` are internal TS-source packages (consumed by the
Electron bundler directly); `apps/desktop` is the Electron app.

## Development

```bash
pnpm install
pnpm dev            # electron-vite dev (apps/desktop)
pnpm test           # vitest across the workspace
pnpm typecheck      # tsc --noEmit everywhere
pnpm build          # turbo build
```

ffmpeg/ffprobe: the app prefers a bundled copy in `apps/desktop/bin/<platform>-<arch>/` (see
`scripts/fetch-ffmpeg.mjs`); otherwise it falls back to `ffmpeg`/`ffprobe` on `PATH` for
development.

## License

MIT — see `LICENSE`. Third-party components are tracked in `THIRD_PARTY_LICENSES.md`.
