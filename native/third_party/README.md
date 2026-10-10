# Vendored sources

Small, permissively licensed dependencies, copied as source. Each folder keeps its LICENSE.

| Folder | What | Version | URL | License |
|---|---|---|---|---|
| `miniaudio/` | single-header audio device I/O (WASAPI only here) | 0.11.22 (2025-02-24) | https://github.com/mackron/miniaudio | MIT-0 / public domain |
| `clap/` | CLAP plugin API headers (`include/` only) | 1.2.6 | https://github.com/free-audio/clap | MIT |

EBU R128 loudness is implemented in `src/audio/loudness.cpp` (no libebur128). VST3 hosting is not included.
