# ML sidecar

Hybrid approach (docs/DECISIONS.md #2): **ONNX Runtime + whisper.cpp native first**; a
PyInstaller-bundled Python worker is added later only for capabilities without native
ports (beat tracking via librosa/madmom is the likely candidate).

Layout (filled in by the ingestion milestone):

```
sidecars/ml/
  models/            downloaded model files (gitignored; managed by the model manager)
  manifest.json      model catalog: name, size, sha256, url, license, min-ram
  src/               worker entry: ONNX Runtime sessions (embeddings, masks, bg removal)
```

The app spawns this worker on demand (packages/platform `PlatformProcesses`), feeds it
file paths only (addendum §5.4: sidecars get the paths they need, nothing else), and
streams NDJSON progress back over stdout.
