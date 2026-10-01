# Captions (paste into chat)

```
Add captions to everything I say:

- Style: karaoke (active word highlighted) — or "bold" / "serif"
- Max [4] words per card
- Placement: [near the bottom, around 82% down]
- Size: [72]px, white with a black outline
- Match the transcript timing exactly

After adding, capture a frame at [2s] so I can check placement, then adjust size or
position if it overlaps anything important.
```

## Batch (across projects via MCP)

With a folder of projects, drive them over MCP (Claude Code / Codex):

```
For each project in [folder]: open it, add karaoke captions, duck the music to 25%
under speech, and export TikTok 1080x1920. Report each project's duration and export
path. Don't change the edit order.
```

## Notes

- Caption word timings come from whisper.cpp word-level ASR, so they follow the audio.
- Captions are normal timeline items — you can restyle or delete them by hand too.
- Motion graphics (lower thirds etc.) are edited only via chat: see `motion-graphics.md`.
