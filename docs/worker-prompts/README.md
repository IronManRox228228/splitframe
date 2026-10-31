# Worker prompts (31 Oct 2026)

Prompts handed to headless coding agents for the loose-ends sweep (see `docs/SWEEP-2026-10-31.md`). Run from the repo root on branch `comfy`, e.g.

    opencode run -m "opencode/deepseek-v4.1-flash-free#high" --auto "$(cat docs/worker-prompts/batch2-sweep-fixes.txt)"

- `sweep-audit.txt`: the read-only audit that produced the findings.
- `batch2-sweep-fixes.txt`: clip labels, agent undo label, agent domains, agent.specialists schema, music.structure null check, project.open file://, DBG print, canvas preset undo, locked-track split/ripple. Status: running on nibbles when the laptop was packed; check `git log` for what landed.
- `batch3-templates-settings-wav.txt`: audio-only export greys out picture settings, ComfyUI auto-start/models-dir settings UI, built-in Z-Image / ACE-Step / LTX templates. Written for agy running in parallel (no build, forbidden-file list); when run alone, drop the "another worker" rule and build + ctest. Templates need the models under `%LOCALAPPDATA%\SplitFrame\comfy\models` to verify, so they are best done on the 4060 laptop.
