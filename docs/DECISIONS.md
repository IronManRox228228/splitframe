# Decision Record

Open decisions from the desktop addendum §8, resolved. The desktop addendum wins over the
main build prompt wherever they conflict.

| # | Decision | Choice | Notes |
|---|----------|--------|-------|
| 1 | Shell | **Electron** | Confirmed by user. Real Chromium everywhere (WebCodecs, sandboxed iframes, canvas compositing), Node main process for the tool registry / MCP server / job manager, easy ffmpeg + ML sidecar management. Hidden behind `packages/platform`. |
| 2 | ML sidecar | **Hybrid** | Confirmed by user. ONNX Runtime + whisper.cpp native first; a PyInstaller-bundled Python worker is added later only for capabilities without native ports (e.g. librosa/madmom beat tracking). |
| 3 | Minimum hardware | Apple Silicon 16 GB (dev/primary), Windows with 8 GB VRAM for GPU features. **CPU-only must work acceptably**: whisper.cpp CPU builds (tiny/base/small), ffmpeg software encode, ONNX CPU EP. Features degrade (slower ASR, software export), never break. |
| 4 | Renderer | **Custom compositor** | Confirmed by user. One WebCodecs/canvas render path shared by preview and export, behind the `Renderer` interface in `packages/renderer`. No Remotion dependency ⇒ no license constraint. Motion-graphics component API is our own small Remotion-style surface (`useCurrentFrame`, `interpolate`, `spring`, `Sequence`, `AbsoluteFill`). |
| 5 | ffmpeg licensing | **LGPL builds** shipped per OS/arch (no GPL components: no libx264/x265 in the bundle). H.264 encode via hardware encoders (VideoToolbox on macOS, NVENC/QSV/AMF on Windows/Linux) with OpenH264 and SVT-AV1 as software fallbacks. Dev machines may use a system ffmpeg (e.g. Homebrew GPL build) — the app detects and records `ffmpegBuildInfo`. Patent pools for H.264/H.265 distribution are a shipping/publisher concern, documented in `THIRD_PARTY_LICENSES.md`, not solved in code. |
| 6 | Name / license / signing | Repo: **openCardboard**. Product brand: **Cutboard** (deliberately distinct from the reference product's "Cardboard" brand per the legal rules; npm scope `@cutboard/*`; rename freely). License: **MIT**. Signing: v1 ships unsigned/ad-hoc dev builds; electron-builder config reads `CSC_LINK`/`CSC_IDENTITY_AUTO_DISCOVERY=false` and no-ops without certs. Notarization (Apple Developer ID) and a Windows code-signing cert are user-provided when they choose to pay for them. |
| 7 | Remote-tunnel MCP | **Not shipped in v1.** Local loopback MCP (127.0.0.1 + bearer token + stdio shim) is the supported path. Documented instructions for a user-run tunnel may be added later behind an experimental flag. |

## Reconstructed scope notes

The main prompt was provided in full; nothing was reconstructed by guesswork. Where the two
documents overlap, this project implements:

- Main §1 product spec **minus** §1.3 storage modes (desktop v1 drops cloud storage mode and
  "back up to cloud"; originals are referenced in place, with optional copy-into-project).
- Main §4 data model **minus** `ownerId`/accounts/`Usage` billing fields; `storageMode` is
  dropped from Project, assets carry a local `path` instead of `storageKey`. `sourceOutFrame`
  is derived (`sourceIn + durationFrames * speed`) rather than stored.
- Main §5 ingestion, §6 tool catalog, §7 motion-graphics sandbox, §10.1/§10.2 design language
  and editor UI: unchanged in intent (MCP/UI specifics per addendum §4 and the marketing site
  shipped separately if at all).
- Main §2 architecture → addendum §2 (main process is the source of truth).
- Main §3 stack → addendum §3. Main §8 MCP → addendum §4. Main §9 render → addendum §3 render
  row + milestone 9. Main §11 billing → dropped. Main §12 repo → addendum §6. Main §13
  milestones → addendum §7.

## Modeling notes worth remembering

- **Source frames are in project-fps timebase.** `item.sourceInFrame` and `speed` are
  expressed in project-fps frames; converting to source *seconds* happens at the media layer
  (`seconds = sourceFrame / projectFps`). This keeps ops self-consistent regardless of an
  asset's native fps — one timeline second always advances the source by `speed` seconds.
- **Project bundle layout**: v1 keeps one central SQLite database (userData/cutboard.sqlite)
  with per-project folders under `~/Movies/Cutboard/<slug>-<id8>/` holding `cache/` (proxies,
  thumbnails, waveforms) and `exports/`. Migrating to fully portable per-project
  `project.db` bundles (addendum §2) is deferred to the packaging milestone; nothing in the
  schema blocks it.
- **Preload must be CommonJS**: Electron cannot load ESM preload scripts into sandboxed
  renderers; electron-vite is configured to emit `out/preload/index.cjs`.
- **Media events are unreliable in hidden windows** (export renderer): media loading and
  seeking poll state instead of waiting on `loadeddata`/`seeked` events.
