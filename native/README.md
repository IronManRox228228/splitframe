# SplitFrame native (C++20 / Qt 6 / FFmpeg)

The native rewrite of SplitFrame. The Electron app in `apps/desktop` stays as the reference
implementation until this one reaches parity; both read and write the same project documents.

## Build (Windows)

Needs Visual Studio 2022 Build Tools (C++ workload), CMake 3.25+, Qt 6.8 (`msvc2022_64` with
qtmultimedia, qtshadertools, qtimageformats) and a shared **GPL** FFmpeg build (BtbN `gpl-shared`: libx264 / libx265 are needed for export; the native app is GPL-3.0-or-later, so GPL FFmpeg is fine here).
Defaults: `QT_ROOT=C:\Qt\6.8.3\msvc2022_64`, `FFMPEG_ROOT=C:\dev\ffmpeg-8.1-gpl`, `OCIO_ROOT=C:\dev\ocio` (OpenColorIO is optional, see Colour management).
Set those environment variables first if your copies live elsewhere.

### One click

Double-click `run_build.bat` in the repo root. It configures the `debug` preset if
`native\build\debug\CMakeCache.txt` does not exist, builds with `-j 8`, then starts
`native\build\debug\bin\splitframe.exe`. If the build fails it stays open so you can read the first error.

### By hand

Run from `native\`. Presets: `debug`, `release` (RelWithDebInfo), `asan`.

```
scripts\dev.cmd cmake --preset debug
scripts\dev.cmd cmake --build --preset debug
scripts\dev.cmd ctest --preset debug
scripts\dev.cmd C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --qmldir src\app --no-translations build\debug\bin\splitframe.exe
build\debug\bin\splitframe.exe clip.mp4
build\debug\bin\splitframe.exe --screenshot out.png clip.mp4   # renders, saves the window, quits
```

`scripts\dev.cmd` runs a command inside the MSVC x64 developer environment with Qt, FFmpeg and OpenColorIO on `PATH`.

The build presets in `CMakePresets.json` use 4 jobs (MSVC uses ~0.5 GB per job, and the local model server shares the
machine). `run_build.bat` overrides that with `-j 8`. Pass `-j <n>` to `cmake --build` to change it.

## Layout

| Path | What | Depends on |
|---|---|---|
| `src/core` | Project model: time, schema types, JSON load/save, ops, undo history, snapping. No GUI. | QtCore |
| `src/media` | FFmpeg: probe, decode (hardware where available), frame cache, audio decode. | core, FFmpeg |
| `src/render` | Compositor on QRhi (D3D11 today): one timeline frame to a texture, zero-copy D3D11VA video. Used by preview and export. | core, media |
| `src/audio` | Mixer, effects, meters, output device. | core, media |
| `src/export` | Render + encode to file (NVENC when available, else software; QSV/AMF requests map to that same path). | render, audio |
| `src/agent` | LLM agents: router, specialists, peer bus, harness over any OpenAI-compatible LLM server (`SF_LLM_*` seam, e.g. llama-server). Four modes: Plan (a plan card, run with `agent.plan.run`), Ask before edits (proposals held until `agent.plan.run` or `agent.plan.skip`), Default, Auto. | core, editor, engine |
| `src/editor` | Editing logic without Qt Quick: `Project` (document, undo history, selection, save/open/autosave/recovery), `MediaPool`, timeline models, `TimelineController` (drag/trim/split/delete as ops), `Inspector`. | core, media |
| `src/engine` | Headless engine (`sf_engine`): one typed command surface over open projects, local JSON-RPC API (HTTP + SSE + stdio), MCP server, `splitframe-cli`. | editor, export, render, audio |
| `src/app` | Qt Quick UI: window, QML panels, painted `TimelineView`, `Editor` facade, `Player`. | everything |
| `tests` | QtTest, one executable per area; `ctest` runs all. | |

## Rules

- C++20, RAII only: no owning raw pointers, no `new`/`delete` outside Qt parent ownership.
  FFmpeg objects live in the `sf::av::*Ptr` owners from `media/ffmpeg.h`.
- Warnings are errors (`/W4 /WX`). An `asan` preset builds with AddressSanitizer.
- Timeline time is integer frames, analysis time is integer ms — same as `packages/schema`.
  Rounding follows JavaScript `Math.round` so both apps compute identical frames.
- Every module gets QtTest coverage; behaviour ported from TypeScript ports its tests too.

## Colour management (C1)

The compositor blends in scene-linear light on an RGBA16F canvas (`src/render/color.h`, `color_manager.h`,
`shaders/layer.frag`, `shaders/output.frag`); `project.colorManagement.blendSpace = "display"` reproduces the
Electron app's gamma blending. OpenColorIO 2.x is optional: build it once (shared, Release, MSVC x64; apps, docs,
Python, tests off; `-DOCIO_INSTALL_EXT_PACKAGES=ALL` fetches its dependencies) into `C:\dev\ocio`, or point `OCIO_ROOT`
at another prefix. `scripts\dev.cmd` puts `%OCIO_ROOT%\bin` on PATH and the build copies the DLL next to the binaries.
Without it only the built-in sRGB / BT.709 / BT.1886 / PQ / HLG pipeline is available.

## Engine, local API, MCP and CLI (M5a)

`src/engine` is the machine front end. `Engine::call(name, args) -> Result` runs one of its registered commands (`engine.commands` /
`splitframe-cli commands` list them with JSON schema, `domain` = editor | colour | audio | engine, and `mutates`); errors are values
(`{code, message, details}`), mutating commands take an `origin` recorded in the history entry. Edits are core ops, so undo/redo and the
Electron app's op log semantics are unchanged.

- `splitframe-cli` (console, no window): `info`, `timeline`, `apply <ops.json> [--save|--out]`, `render-frame`, `export` (the `--export` code),
  `call <command> [json]`, `commands`, `serve [--stdio]`, `mcp`; `--json` for machine output. Exit codes: 0 ok, 1 command failed, 2 usage,
  3 export cancelled, 4 project can't be opened.
- `splitframe-cli serve` / the GUI (`splitframe.exe --api [--api-port n]` or File > Local API): JSON-RPC 2.0 on `POST /rpc` and Server-Sent Events on
  `GET /events`, 127.0.0.1 only. Every request needs `Authorization: Bearer <token>` (random per start, written to
  `<AppConfigLocation>/api-token`, i.e. `%LOCALAPPDATA%/SplitFrame/SplitFrame/api-token`); foreign `Host` / `Origin` headers are rejected.
- `splitframe.exe --mcp [project.json]` or `splitframe-cli mcp`: MCP over stdio (tools = the engine commands with `.` written `_`, resources
  `splitframe://timeline|project|assets|frame/{n}`).

## AI / LLM settings

The agent crew talks to any OpenAI-compatible chat server. Each setting is read from its environment variable first; if that is
empty, from the Windows registry (`HKCU\Software\SplitFrame\SplitFrame`, written by the in-app AI settings dialog) under the same key name.

| Name | Meaning | Default |
|---|---|---|
| `SF_LLM_ENDPOINT` | Full chat-completions URL. SplitFrame posts to the URL exactly as given and appends nothing, so include `/v1/chat/completions`. The value `mock` runs the crew offline. | `http://127.0.0.1:8080/v1/chat/completions` |
| `SF_LLM_MODEL` | Model name sent with each request. | `qwythos-9b-v2` |
| `SF_LLM_MODEL_FAST`, `SF_LLM_MODEL_DEEP`, `SF_LLM_MODEL_JUDGE` | Model per lane. Order: env var, same-named setting, `ai/modelFast` / `ai/modelDeep` / `ai/modelJudge` setting, then `SF_LLM_MODEL`. | `SF_LLM_MODEL` |
| `SF_LLM_API_KEY` | Sent as a Bearer token to the endpoint above. Only needed if the server asks for one. | none |
| `SF_LLM_TIMEOUT_MS` | Timeout of one completion request, in ms. Applies only if above 0. | no limit (Stop still aborts) |
| `SF_LLM_MOCK_SCRIPT` | Path to a JSON file that scripts the replies when the endpoint is `mock`. Environment variable only (not read from the registry). | none |

Examples (PowerShell; start SplitFrame from the same window):

```
# Local llama-server running Qwythos v2 9B
$env:SF_LLM_ENDPOINT = "http://127.0.0.1:8080/v1/chat/completions"
$env:SF_LLM_MODEL    = "qwythos-9b-v2"

# Google Gemini through its OpenAI-compatible endpoint
$env:SF_LLM_ENDPOINT = "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions"
$env:SF_LLM_MODEL    = "gemini-3.5-flash-lite"
$env:SF_LLM_API_KEY  = "<your-key>"
```

The Gemini example is the cloud setup the crew was tested with end to end. `splitframe-cli` also accepts
`--llm-endpoint <url|mock>`, `--llm-model <name>` and `--llm-mock-script <file>` after the subcommand name, and `splitframe-cli agent "<request>"` runs the crew from the command line.
The app's AI settings dialog edits the same settings (the API key is stored in the registry and shown masked).
Before an agent turn SplitFrame checks that the endpoint's host and port accept a connection. With no model reachable, the AI commands answer error code `unavailable`.

## Generate with ComfyUI (open weights)

### What it does
Generated shots (image, video, and audio) land directly on the timeline, where the editor's deterministic layers cut, grade, beat-sync, safety-check, and export them. Variants of a shot can be rendered as a contact sheet and picked by letter (e.g., variant `A`, `B`, `C`, etc.) in a single undo step.

### How SplitFrame finds ComfyUI
SplitFrame uses one of two modes (`src/comfy/comfy_process.cpp`):

- **External:** if `SF_COMFY_URL` (or the registry value `comfy/url`) is set, SplitFrame talks to the ComfyUI already running at that URL and never starts or stops it.
  If it is not set, the client uses `http://127.0.0.1:8188`.
- **Managed:** if `SF_COMFY_HOME` and/or `SF_COMFY_PYTHON` (or registry `comfy/home`, `comfy/python`) are set and no URL is, SplitFrame starts ComfyUI itself:
  `python -s main.py --listen 127.0.0.1 --port <port> --disable-auto-launch`, run in the ComfyUI folder. The port is a free local port that SplitFrame picks each time, not a fixed one.
  It waits up to 180 s for the server, and stops it when told to or when SplitFrame exits.
  If no Python is given it looks for `..\python_embeded\python.exe`, then `venv\Scripts\python.exe`, then `.venv\Scripts\python.exe`, relative to the ComfyUI folder.
  Registry `comfy/autoStart` (default false) starts it with the engine and restarts it after a crash, at most twice in 10 minutes.
  Commands: `comfy.server.start` (`home`, `python`, `args`, `save`, `timeoutMs`), `comfy.server.stop`, `comfy.server.restart`, `comfy.server.status`, `comfy.server.logs`.
  The server's output is also written to `comfy\comfy-<date>.log` in the app data folder.

The AI settings dialog has the same ComfyUI fields (URL, folder, Python) and a Test button.

#### ComfyUI folder layout used on the dev machine
SplitFrame's source does not install ComfyUI or fetch anything. The development machine keeps its own ComfyUI under `%LOCALAPPDATA%\SplitFrame\comfy\`:

```
%LOCALAPPDATA%\SplitFrame\comfy\
  runtime\ComfyUI\          ComfyUI itself
  runtime\python_embeded\   embedded Python (found automatically next to the ComfyUI folder)
  models\                   checkpoints, diffusion_models, text_encoders, vae, latent_upscale_models, tts
  workflows\                API-format workflow JSON
```

To use that layout, set `SF_COMFY_HOME=%LOCALAPPDATA%\SplitFrame\comfy\runtime\ComfyUI`. A ComfyUI Windows portable build has the same
`ComfyUI` + `python_embeded` side-by-side shape. Which model files you need depends on the workflow you import, and this repo ships no workflows or weights.
`comfy.ready` and `generate.ready` report any model file a template needs that the running ComfyUI cannot see, and the `models` subfolder it expected it in.

### Requirements
- A ComfyUI server (any recent version): one you start yourself, for example
  ```bash
  python main.py --listen 127.0.0.1 --port 8188
  ```
  (then set `SF_COMFY_URL=http://127.0.0.1:8188`, or leave it unset to use that default), or one SplitFrame manages (above).
- Open-weights models that you install yourself (for example LTX video, Z-Image, ACE-Step). SplitFrame never downloads model weights; it only downloads the output files ComfyUI renders.
- A workflow template per kind of shot. No templates are built in at the moment (`src/engine/comfy_templates` holds only its README). Import one with `comfy.template.import` (below); that folder's README has image, video, speech and music examples.

### Configuration
Environment variables:
- `SF_COMFY_URL`: ComfyUI server URL for external mode (default when unset: `http://127.0.0.1:8188`).
- `SF_COMFY_HOME`, `SF_COMFY_PYTHON`: ComfyUI folder and Python for managed mode.
- `SF_COMFY_TEMPLATES`: Directory for user-imported workflow templates. Without it, templates are saved in a `comfy_templates` folder in the app data location.
- `SF_COMFY_OBJECT_INFO`: Optional path to an offline `/object_info` schema dump (used by the test suite and offline environments).

### Adding your own workflow
In ComfyUI, enable "Dev mode Options" in the settings, then click **Export (API)** to save your workflow in API format JSON. Then import the workflow into SplitFrame using `splitframe-cli call comfy.template.import` with typed parameter bindings.

Worked example for an image-to-video workflow:
```bash
splitframe-cli call comfy.template.import '{
  "id": "my_i2v_workflow",
  "name": "Custom Image-to-Video",
  "kind": "video",
  "description": "Generate video from an initial image using open weights",
  "whenToUse": ["b-roll", "cutaway", "transition"],
  "path": "C:/path/to/workflow_api.json",
  "outputNode": "12",
  "bindings": {
    "prompt": { "node": "6", "input": "text" },
    "seed": { "node": "3", "input": "seed" },
    "image": { "node": "10", "input": "image" },
    "frames": { "node": "3", "input": "length" },
    "fps": { "node": "12", "input": "frame_rate" }
  }
}'
```
*Note:* The generic node IDs (`"6"`, `"3"`, `"10"`, `"12"`) in this example must match the corresponding node IDs in your exported ComfyUI API JSON file.

### CLI walkthrough
All operations are available via `splitframe-cli call`:

1. **Check server status:**
   ```bash
   splitframe-cli call comfy.status
   ```
2. **List available templates:**
   ```bash
   splitframe-cli call comfy.template.list '{"kind":"video"}'
   ```
3. **Generate a shot onto the timeline:**
   ```bash
   splitframe-cli call generate.shot '{
     "session": "s1",
     "templateId": "my_i2v_workflow",
     "prompt": "wide drone sweep over morning mist in the redwoods",
     "seed": 42,
     "trackId": "track-v1",
     "atFrame": 0
   }'
   ```
4. **Render alternative variants (contact sheet):**
   ```bash
   splitframe-cli call reshoot.variants '{
     "session": "s1",
     "itemId": "item_1",
     "count": 4,
     "vary": "seed"
   }'
   ```
5. **Pick a preferred variant from the contact sheet:**
   ```bash
   splitframe-cli call reshoot.pick '{
     "session": "s1",
     "itemId": "item_1",
     "letter": "B"
   }'
   ```
6. **Agent-friendly workflow graph editing with validation:**
   ```bash
   # Load an API-format workflow into memory (returns a graphId, e.g. "g1")
   splitframe-cli call comfy.graph.load '{"path":"workflow_api.json"}'

   # Apply typed graph operations atomically with undo tracking
   splitframe-cli call comfy.graph.op '{
     "graphId": "g1",
     "ops": [
       {"op": "set", "id": "3", "input": "steps", "value": 30},
       {"op": "set", "id": "6", "input": "text", "value": "golden sunset over ocean cliffs"}
     ]
   }'

   # Validate graph structure and input types against the schema
   splitframe-cli call comfy.graph.validate '{"graphId":"g1"}'

   # Enqueue execution as a background job
   splitframe-cli call comfy.run '{"graphId":"g1","wait":true}'
   ```

### How it's built
- **Dynamic tool schemas:** Node tool schemas are introspected directly from ComfyUI's `/object_info` endpoint.
- **Typed graph operations with undo:** In-memory graph documents support atomic batch ops (`add`, `remove`, `set`, `connect`, `disconnect`, `clear`) with an undo stack.
- **Deterministic validator:** A 10-error-code validator (`unknown_class`, `unknown_input`, `missing_required`, `bad_link`, `type_mismatch`, `bad_enum`, `out_of_range`, `bad_literal_type`, `cycle`, `no_output_node`) runs before any graph is queued.
- **Content-addressable caching:** Generation requests are hashed by graph contents, seed, and input parameters to avoid redundant runs.
- **Resource management:** Background generation jobs automatically pause during export to prioritize render and encode pipelines.
- **Deterministic calculations:** Every timing offset, frame rate conversion, and layout coordinate is computed by deterministic C++ code, not model hallucinations.

### License
SplitFrame is licensed under [GPL-3.0-or-later](../LICENSE). Generated media files follow the licenses of whichever model weights and checkpoints the user runs in their local ComfyUI installation.

