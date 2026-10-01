# Rough cut (paste into the built-in chat or any MCP client)

Copy this into the Cutboard chat (or your MCP client connected to the local server) and
fill in the bracketed parts. The agent works in small steps and verifies with
`getTimeline` + `captureFrame` — you'll see its tool calls as cards in the chat.

```
Build a rough cut of my project:

- Target length: [45 seconds]
- Platform: [TikTok / Reels / YouTube]
- Footage: all my clips, keep their current order
- Keep: [the last take of each repeated line, the cleanest delivery]
- Strip: pauses over [0.5] seconds, false starts, dead air before the first word
- Ending: hold on the last shot for a moment before it ends

Work step by step: read the transcripts, cut the silences with removeSilences
(threshold [0.5s]), drop retakes with buildRoughCut, then check the duration with
getTimelineDuration and tell me how long the result is.
```

## Notes

- All edits are one undo per macro (⌘Z undoes the whole rough cut).
- If the transcript is missing, import audio-bearing footage first — ASR runs on import.
- For beat-driven pacing instead, see `beat-sync.md`.
