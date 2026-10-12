# SplitFrame native — hand-test checklist (M1 → M5a)

Nothing below has been hand-tested; only ctest has run. Flags and shortcuts were read
out of the source, not guessed. Generated 2026-10-10.

## Build and run

```
scripts\dev.cmd cmake --preset debug
scripts\dev.cmd cmake --build --preset debug
scripts\dev.cmd ctest --preset debug
scripts\dev.cmd C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --qmldir src\app --no-translations build\debug\bin\splitframe.exe
build\debug\bin\splitframe.exe clip.mp4
build\debug\bin\splitframe.exe --screenshot out.png clip.mp4
```

Do not run `windeployqt` at `build\debug\bin` twice — stale qml/platforms broke UI tests before.

App flags: `--frame n`, `--play <sec>`, `--cpu`, `--sw`, `--settings x.ini`,
`--recovery-dir d`, `--select 0,2`, `--fit`, `--api [--api-port n]`, `--mcp [project.json]`, `--export ...`

## M1 — project model and media engine

- Open `native/tests/fixtures/docs/rich.json` via File > Open; clips render, no crash.
- Open the NTSC and odd-fps fixtures; frame counts match what Electron shows.
- Import an H.264 clip and an HEVC/AV1 clip; both probe and show thumbnails (AV1/VP9 hw path untested).
- Import a 5.1 clip; the waveform draws (downmix untested).

## M2 — QRhi compositor, zero-copy D3D11, preview

- Space plays, K pauses, J/L shuttle, Left/Right step a frame, Shift+Left/Right a second, Home/End jump.
- `--play 10`, read the pacing numbers on stderr, note dropped frames.
- Run with `--cpu`, then `--sw`; compare both against the default path visually.
- Two overlapping clips with transform/opacity; preview must match `--screenshot` output.
- Watch GPU memory while playing. llama-server holds ~7.8 of 8.2 GB, so an OOM or device-lost here is real.

## M3 — timeline UI, media pool, inspector, lifecycle

- Drag a clip past the end; the sequence extends, nothing overlaps.
- Ripple on (menu action "Ripple edits", no key bound): delete a middle clip, later clips move left.
- Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z after each edit; undo must be exact.
- `S` splits at playhead; Delete/Backspace removes; Ctrl+D clones; Alt+Left/Right nudges 1 frame, Alt+Shift+Left/Right 10.
- `N` toggles snapping: drag an edge near another with it on, then off.
- Trim an edge; inspector values update in frames.
- Edit an inspector field then Escape; Escape deselects and must not half-commit.
- Ctrl+N with unsaved changes prompts; then Ctrl+O, Ctrl+Shift+S, Ctrl+S; reopen; recent files list.
- Force-kill mid-edit and relaunch; recovery offers the work back (use `--recovery-dir` to isolate).
- Zoom: `=`/`+`/Ctrl+= in, `-`/Ctrl+- out, Shift+Z zoom to fit. Up/Down jump clip edges.

## C1 — linear-light compositor and OCIO

- `colorManagement.blendSpace = "display"` vs default linear: overlap 50% gray on black.
  Default (linear) should be brighter at the blend edge; "display" should match Electron.
- Build without `C:\dev\ocio`: built-in sRGB/BT.709/BT.1886/PQ/HLG must still work.
  With OCIO present, confirm it loads and its DLL sits next to the binary.
- Feed a PQ/HLG source; it must not clip to white (tone map untested).
- There is no colour UI yet, so changing spaces is JSON-only.

## M4a — audio engine, mixer, effects, loudness, CLAP

- Play a clip with audio; check sync over 60 s and that it uses the default device.
- Change volume/fade in the inspector during playback; no click, no dropout.
- Load the test CLAP gain plugin (`tests/clap_test_gain.cpp`); level changes; unload without crash.
- `splitframe-cli call audio.analyze '{...}'`; loudness should land within ~1 LU of a reference tool.
- No master meter UI yet. Speed changes pitch-shift (no pitch-preserving stretch) — expected.

## M4b — export, headless CLI, proxies

```
splitframe-cli export proj.json --out out.mp4 --preset 1080p --quality standard --codec h264 --crf 20 --overwrite
```

Other flags: `--list-presets`, `--container mp4|mov|mkv|wav`, `--audio-codec aac|pcm16|pcm24|pcm32f|none`,
`--no-audio`, `--audio-only`, `--encoder-preset medium|p1..p7`, `--bitrate <kbps>`, `--width`, `--height`,
`--range in:out`, `--ten-bit`, `--prores-profile proxy|lt|standard|hq|4444`,
`--dnx-profile dnxhr_lb|dnxhr_sq|dnxhr_hq|dnxhr_hqx|dnxhr_444`, `--lufs`, `--true-peak`, `--hw` (NVENC), `--hw-decode`.
Also `splitframe.exe --export proj.json --out out.mp4`.

Exit codes: 0 ok, 1 failed, 2 usage, 3 cancelled, 4 project won't open.

- Try `--codec prores --prores-profile hq --container mov`, `--codec dnxhr --dnx-profile dnxhr_hq --container mov`,
  and `--container wav --audio-only`.
- GUI: Ctrl+E opens the export dialog. Export, cancel mid-way (exit 3, check for partial files), export again.
- Proxies: `proxies.request` / `proxies.status` on a 4K clip; playback switches to the proxy. Proxy audio is deferred.
- NVENC and the export dialog have never run outside ctest.

## M5a — headless engine, JSON-RPC, MCP, CLI

- `splitframe-cli info|timeline|render-frame proj.json`, and `splitframe-cli --json commands`.
- `splitframe-cli apply proj.json ops.json --out out.json` → exit 0; then a bad op → exit 1, file unchanged.
- `splitframe-cli serve <project> --port 8123`
  (token at `%LOCALAPPDATA%\SplitFrame\SplitFrame\api-token`)

```
curl -X POST http://127.0.0.1:8123/rpc -H "Authorization: Bearer <token>" -H "Content-Type: application/json" ^
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"timeline.get\",\"params\":{}}"
```

- Repeat with no bearer (expect 401), with `Origin: http://evil.example` (403), with a bad Host (403).
- `project.open`, then `clip.add` / `item.move` / `history.undo`; the open GUI must reflect it (never auto-tested).
- SSE: `curl -N http://127.0.0.1:8123/events?events=export.progress` while `export.start` runs.
- MCP: `splitframe-cli mcp` over stdio — `tools/list`, then `tools/call` on `timeline_get`. Also `splitframe.exe --mcp proj.json`.

## Known gaps — do not file these as bugs

- **M1:** caption split through a word gives a negative `startMs` (TS parity bug, kept deliberately, fix in both apps).
  Unknown JSON fields stripped on save; validation stops at the first error; `item.update` stores defaulted props.
  Untested: hw AV1/VP9, HDR tone map, 5.1 downmix, ASan.
- **M2:** no masks or feather, no blur, no crop/fit modes, no motion-graphic JS, no device-lost recovery,
  no non-D3D11 backends. Qt 6.8.3 crashes if our own D3D11 device is imported into Qt Quick, so the app
  adopts Qt's device for decode.
- **M3:** masks and keyframed drag were never checked in the running app; no mask UI in the inspector.
- **C1:** no colour UI; OCIO output spaces absent from export.
- **M4a:** no master meter UI, no pitch-preserving stretch, no per-node latency compensation, no VST3.
- **M4b:** no WebM; proxy audio deferred; NVENC and the export dialog unverified at runtime.
- **M5a:** GUI attach not auto-tested; `project.color.set` is not undoable; `media.import` waits on a nested
  event loop; `frame.render` not tuned for repeated renders; `captions.add` needs explicit words (no ASR).

## Highest-risk untested areas, ranked

1. **Zero-copy D3D11 preview and Qt 6.8.3 device adoption** — crash or device-lost on play, scrub and
   window resize, especially while llama-server holds ~8 GB of VRAM.
2. **Export end to end** — GPL FFmpeg, x264/x265, ProRes/DNxHR, and the cancel path. No real export has run.
3. **Audio on real hardware** — sync drift, dropouts, device changes, CLAP load/unload crashes.
4. **The local JSON-RPC and MCP surface** — token handling, Host/Origin rejection, and whether RPC edits
   actually appear in an open window.
5. **Undo and ripple parity with Electron** on real projects; fixtures are unit-tested only, so ripple and
   trim across tracks and fractional-speed clips may diverge.
