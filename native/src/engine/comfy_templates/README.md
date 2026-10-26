# ComfyUI Workflow Templates

This directory contains built-in workflow templates for SplitFrame's ComfyUI integration.

## Creating and Importing Custom Templates (e.g. LTX Video)

1. **Build and test your workflow in ComfyUI:**
   - Create your workflow in the ComfyUI web UI (for example, an LTX-Video text-to-video or image-to-video setup).
   - Ensure you enable "Enable Dev mode Options" in the ComfyUI settings dialog.

2. **Export the workflow in API format:**
   - In the ComfyUI menu, click **Save (API Format)** or **Export (API)** to download the API JSON file.
   - Note the node IDs for the prompt, seed, dimensions, frame count, input image, and the final output node (such as `SaveAnimatedWEBP`, `SaveImage`, or `VHS_VideoCombine`).

3. **Import the template into SplitFrame:**
   Use the `comfy.template.import` engine command:
   ```json
   {
     "id": "ltx_video",
     "name": "LTX Video text to video",
     "kind": "video",
     "description": "Generate video using Lightricks LTX-Video model",
     "whenToUse": ["b-roll", "establishing", "action"],
     "path": "/path/to/ltx_api.json",
     "outputNode": "12",
     "bindings": {
       "prompt": {"node": "6", "input": "text"},
       "negative": {"node": "7", "input": "text"},
       "seed": {"node": "3", "input": "seed"},
       "width": {"node": "5", "input": "width"},
       "height": {"node": "5", "input": "height"},
       "frames": {"node": "5", "input": "length"}
     }
   }
   ```
   SplitFrame validates that all referenced node IDs and inputs exist, verifies the output node, and saves the template to the user's template directory (`SF_COMFY_TEMPLATES` or app data location).

## Speech (TTS) Workflow Export and Import

Core ComfyUI does not ship a built-in Text-To-Speech (TTS) node. To generate speech locally with `generate.speech`:

1. Install a TTS custom node package in your ComfyUI setup, such as:
   - `comfyui-kokoro` (Kokoro-82M TTS)
   - `ComfyUI-Chatterbox-TTS`
   - `ComfyUI-F5-TTS`
2. Construct a TTS workflow in the ComfyUI web UI and enable "Dev mode Options".
3. Save or Export the workflow in API format (`speech_api.json`).
4. Import the template into SplitFrame using `comfy.template.import`:
   ```json
   {
     "id": "kokoro_speech",
     "name": "Kokoro TTS Speech Generation",
     "kind": "speech",
     "outputKind": "audio",
     "description": "Local speech generation via Kokoro-TTS custom node",
     "whenToUse": ["voiceover", "dialogue", "narration"],
     "path": "/path/to/speech_api.json",
     "outputNode": "10",
     "bindings": {
       "text": {"node": "5", "input": "text"},
       "voiceRef": {"node": "5", "input": "voice"}
     },
     "requires": {
       "nodes": ["KokoroTTS", "SaveAudio"]
     }
   }
   ```
5. Call `generate.speech {"templateId": "kokoro_speech", "text": "Your voiceover script here"}`.

## Music Workflow Export and Import

Core ComfyUI does not ship built-in music generation nodes (such as ACE-Step or Stable Audio). To generate music locally with `generate.music`:

1. Install an audio/music custom node package in your ComfyUI setup (e.g. `ComfyUI-ACE-Step` or `ComfyUI-Stable-Audio`).
2. Construct the music generation workflow in the web UI and export it in API format (`music_api.json`).
3. Import the template into SplitFrame using `comfy.template.import`:
   ```json
   {
     "id": "ace_step_music",
     "name": "ACE-Step Music Bed Generation",
     "kind": "music",
     "outputKind": "audio",
     "description": "Local music bed generation from prompt, duration, and BPM",
     "whenToUse": ["soundtrack", "background music", "score"],
     "path": "/path/to/music_api.json",
     "outputNode": "12",
     "bindings": {
       "prompt": {"node": "4", "input": "tags"},
       "durationSec": {"node": "6", "input": "duration"},
       "bpm": {"node": "6", "input": "bpm"}
     },
     "requires": {
       "nodes": ["ACEStepLoader", "ACEStepSampler", "SaveAudio"]
     }
   }
   ```
4. Call `generate.music {"templateId": "ace_step_music", "prompt": "lofi chill beats", "durationSec": 15}`.

## Node Class Names and Custom Nodes

Template graphs reference ComfyUI node classes by their `class_type` string (for example `KSampler`, `CheckpointLoaderSimple`, `SaveImage`). These names must come from one of two sources:

1. **A real ComfyUI install** - the `/object_info` endpoint lists every registered `class_type` for the running server.
2. **An existing template** - classes already used by a shipped template or a workflow you exported from ComfyUI.

Never copy a class name from memory into a template: an unverified `class_type` fails readiness (`comfy.ready` reports it as a missing node) and generation (`generate.*`) refuses to queue the shot.

## Built-in Templates

No templates are built in right now; LTX, Z-Image and ACE-Step templates will be added here. Installed templates are listed by `comfy.template.list`.

## Comfy Router Cloud Provider (Comfy Developer Platform)

SplitFrame provides native C++ client integration for the Comfy Developer Platform via **Comfy Router** (`native/src/comfy/comfy_router_client.{h,cpp}`, `sf::comfy::RouterClient`, and engine commands `comfy.router.*`, `generate.router.*`, and `generate.ready` with `provider: "router"`).

### Official Endpoints Implemented
- Model Catalog: `GET /v2/models?limit&cursor` (cursor-paginated model listing; docs: https://docs.comfy.org/development/comfy-router/quickstart)
- Model Metadata: `GET /v2/models/{provider}/{model}` (GET single partner model entry)
- OpenAPI Schema: `GET /v2/models/{provider}/{model}/openapi.json` (model input/output schema with HTTP ETag / 304 Not Modified support)
- Queue Request: `POST /v2/models/{provider}/{model}/requests` (creates generation job; unique UUID `Idempotency-Key` sent per submit)
- Poll Request: `GET /v2/models/{provider}/{model}/requests/{requestId}/status` (polls queue position / execution state)
- Collect Output: `GET /v2/models/{provider}/{model}/requests/{requestId}` (reads native JSON outputs or inline binary assets saved via `saveAs`)
- Cancel Request: `PUT /v2/models/{provider}/{model}/requests/{requestId}/cancel` (cooperative cancellation)

### Contest Testing Scope & Fake Server
Per the Comfy contest instructions and project scope rules:
- **Fake Server Only:** The cloud Router path is implemented in native C++ and tested against a local fake server (`FakeComfyServer` in `native/tests/comfy_fake_server.h`).
- **Never Called Live:** The cloud Router path is **never called live** in tests or development; there are NO live network requests to `api.comfy.org` and NO real API keys are configured or stored.
- All test suites (`tst_generate`, `tst_comfy`) verify success, HTTP auth errors (401), HTTP 5xx errors (500), transport failures (connection refused), and missing-key paths (clear error, no crash) strictly against the fake server without credentials or external traffic.

