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

Built milestone by milestone (see `docs/DECISIONS.md` and the desktop addendum):

1. [x] **Shell + core** — Electron app, typed IPC, SQLite project service (op log + WAL),
       op-based editor core with exact undo/redo (46 unit tests), manual timeline
       (drag/trim/split/clone/slip/snap/ripple), proxy-based preview, hardware-encode
       export (VideoToolbox/NVENC + filter_complex audio mix; a 3s clip exports in ~2s).
2. [x] **Import + ingestion** — file referencing + relink, proxies/thumbnails/waveforms,
       whisper.cpp word-level ASR, scene detection + keyframes, pluggable VLM scene
       descriptions (Anthropic/OpenAI/Ollama; key-gated, local-only by default), local
       bge-small embeddings + sqlite-vec with FTS5 hybrid search, model manager.
3. [x] **Tool registry + built-in chat** — 30+ typed tools shared by chat and MCP;
       streaming chat (Anthropic/OpenAI/Google/Ollama) with tool-call cards, API keys in
       the OS-encrypted secret store, editor-context awareness, headless `captureFrame`.
4. [x] **Local MCP** — 127.0.0.1 Streamable HTTP, per-install bearer token, Origin/Host
       validation, session revocation, copy-paste snippets for Claude Code / Claude
       Desktop / Codex / Cursor, stdio shim (`apps/mcp-shim`). Wire-verified with curl
       (401/403/initialize/tools-list/tools-call).
5. [x] **Macros** — buildRoughCut, removeSilences, addCaptions (serif/bold/karaoke from
       transcript timings), duckMusic (volume keyframes honored by the exporter).
6. [x] **Beat sync** — local DSP beat detection (BPM/grid/downbeats/sections) + beatSync
       with per-section density maps. Verified: 121 BPM detected on a 120 BPM click
       track; 20 cuts placed on the grid.
7. [x] **Motion graphics** — own Remotion-style scene-tree API, acorn AST deny-list,
       sandboxed-iframe evaluation (no same-origin), shared preview/export canvas
       renderer, create/update/preview tools with a capture-and-repair loop.
8. [x] **Reference style** — cut-rhythm + coarse grade analysis; property-level apply
       (grade effects, pacing targets). Reference footage never enters the output.
9. [x] **Export niceties** — queue with progress/cancel, system notifications, OTIO
       export for Resolve/Premiere/FCP, hardware encoder selection.
10. [~] **Packaging** — electron-builder config (dmg/NSIS/AppImage; mac-arm64 builds
         verified), opt-in electron-updater, bundled ffmpeg/ffprobe via
         `pnpm fetch:ffmpeg`, whisper fetch via `pnpm fetch:whisper`, prompt library
         (`docs/prompts`), automated smoke driver (`CUTBOARD_SMOKE_*`). Signing and
         notarization need user-provided certs; installers build unsigned.

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
