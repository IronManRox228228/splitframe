# SplitFrame native (C++20 / Qt 6 / FFmpeg)

The native rewrite of SplitFrame. The Electron app in `apps/desktop` stays as the reference
implementation until this one reaches parity; both read and write the same project documents.

## Build (Windows)

Needs Visual Studio 2022 Build Tools (C++ workload), CMake 3.25+, Qt 6.8 (`msvc2022_64` with
qtmultimedia, qtshadertools, qtimageformats) and a shared FFmpeg build (BtbN `lgpl-shared`).
Defaults: `QT_ROOT=C:\Qt\6.8.3\msvc2022_64`, `FFMPEG_ROOT=C:\dev\ffmpeg-8.1`.

```
scripts\dev.cmd cmake --preset debug
scripts\dev.cmd cmake --build --preset debug
scripts\dev.cmd ctest --preset debug
scripts\dev.cmd C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe --qmldir src\app --no-translations build\debug\bin\splitframe.exe
build\debug\bin\splitframe.exe clip.mp4
build\debug\bin\splitframe.exe --screenshot out.png clip.mp4   # renders, saves the window, quits
```

Builds run 4 jobs at a time (`CMakePresets.json`): MSVC uses ~0.5 GB per job and this machine also
hosts the local model server.

## Layout

| Path | What | Depends on |
|---|---|---|
| `src/core` | Project model: time, schema types, JSON load/save, ops, undo history, snapping. No GUI. | QtCore |
| `src/media` | FFmpeg: probe, decode (hardware where available), frame cache, audio decode. | core, FFmpeg |
| `src/render` | Compositor on QRhi (D3D11 today): one timeline frame to a texture, zero-copy D3D11VA video. Used by preview and export. | core, media |
| `src/audio` | Mixer, effects, meters, output device. | core, media |
| `src/export` | Render + encode (NVENC/AMF/QSV/software) to file. | render, audio |
| `src/agent` | Harness v1 port: router, facade tools, plans, verifier, modes. llama-server over HTTP. | core |
| `src/app` | Qt Quick UI: media pool, timeline, preview, inspector, chat. | everything |
| `tests` | QtTest, one executable per area; `ctest` runs all. | |

## Rules

- C++20, RAII only: no owning raw pointers, no `new`/`delete` outside Qt parent ownership.
  FFmpeg objects live in the `sf::av::*Ptr` owners from `media/ffmpeg.h`.
- Warnings are errors (`/W4 /WX`). An `asan` preset builds with AddressSanitizer.
- Timeline time is integer frames, analysis time is integer ms — same as `packages/schema`.
  Rounding follows JavaScript `Math.round` so both apps compute identical frames.
- Every module gets QtTest coverage; behaviour ported from TypeScript ports its tests too.
