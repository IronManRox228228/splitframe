# Beat sync (paste into chat)

```
Cut my video to the beat of [the music track I imported]:

- Music: [name of the audio item] (detect its beat grid first — analyzeBeats)
- Density: one clip per beat in high-energy sections, every 2 beats in medium,
  every 4 beats in low / intro sections
- Use my video clips in their current order
- When clips run out, hold the last shot
- Replace the current sequence on the main video track (undo can restore it)

Then add karaoke captions and duck the music to 25% under my speech.
Export when I confirm it looks good.
```

## How it works

- `analyzeBeats` runs a local onset/autocorrelation detector (no cloud, no Python) and
  reports BPM, the beat grid, downbeats, and energy sections.
- `beatSync` places cuts on the grid (±1 frame) with a per-section density map.
- Weak-pulse or ambient tracks: ask for "cut on phrase changes instead" — use
  buildRoughCut pacing rather than the beat grid.

## Reference style

To match a reference video's pacing, first `setReferenceAsset` + `analyzeReferenceStyle`,
then ask: "match the reference's cuts per minute" — the agent will use the measured
pacing as its density target. The reference footage is never placed in your edit.
