# Motion graphics (paste into chat)

```
Create a lower third for when I introduce the topic:

- Text: [Dana Reyes — Ceramics 101]
- Appears at [1.5s], stays for [4s], slides in from the left with a spring
- Style: white heavy Inter, small amber underline bar that scales in after the text
- Bottom-left third of the frame
```

The agent writes a small scene-tree composition with the allowed API, creates it with
`createMotionGraphic`, then calls `previewMotionGraphic` to LOOK at it and repairs itself
if the frame shows an error (max 3 attempts). Tell it what you want changed and it will
`updateMotionGraphic` — graphics are edited only through chat, never drag handles.

## What the API can do (and can't)

Can: layout, text, boxes/ellipses, images from project assets (`Img src:'asset:<id>'`),
opacity/position/scale animation, per-frame math, `Sequence` time-shifting, word-timed
reveals from `inputProps.words` (transcript timings).

Can't: 3D, character animation, particle systems, network requests, arbitrary imports —
the sandbox rejects any code that touches the network, DOM, or dynamic evaluation
(static AST check inside the sandbox host).
