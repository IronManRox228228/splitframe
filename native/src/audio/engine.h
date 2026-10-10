#pragma once

// The timeline audio engine: every audible item of a TimelineDoc mixed through the (optional) mixer
// graph to 48 kHz float stereo. Preview playback and export both run through it.
//
// Semantics follow the Electron app (apps/desktop/src/main/export-plan.ts, packages/renderer
// keyframes.ts): video and audio items contribute unless the item or its track is muted or the asset
// has no audio; the item's volume (or its "volume" keyframes, item-local) is multiplied by linear
// fade in / fade out ramps and capped (RenderOptions::volumeCeiling, 1 as in the export); speed and
// time remap read the source faster or slower with a band-limited resampler (varispeed: pitch follows
// speed; the export's pitch-preserving atempo is not reproduced).
//
// Signal flow, with the `mixer` block absent or present:
//   items -> track strip (inserts -> [pre-fader sends] -> fader -> pan -> [post-fader sends] -> meter)
//         -> bus (same) or master  -> master (inserts -> fader -> pan -> meter) -> out
// Without a block every track is a unity strip into the master, which is what the old export did.
//
// Sample accuracy: item positions are frame boundaries converted by timebase.h (integer, no drift); all
// processing is per sample, so the result does not depend on the block size or on how a range is split
// into render() calls, as long as the calls are contiguous.

#include "audio/loudness.h"
#include "audio/source.h"
#include "core/schema.h"

#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace sf::audio {

struct RenderOptions {
  // Upper bound of an item's linear gain (volume keyframes, fades included). 1 matches the Electron
  // preview and export; raise it to allow clip gain above unity.
  double volumeCeiling = 1.0;
  // The Electron export mixes audio of hidden tracks (hidden only affects the picture); set to silence them.
  bool hiddenTracksSilent = false;
  // renderRange only: render this many samples before `start` (discarded) to warm up effect state.
  std::int64_t prerollSamples = 0;
  int blockSize = 512;
};

struct NodeMeter {
  float peak[2] = {0, 0};     // last block, linear
  float peakHold[2] = {0, 0}; // falls 20 dB/s
  float rms[2] = {0, 0};      // last block
  float gainReductionDb = 0;  // strongest reduction among the node's dynamics inserts
};
// Keyed by node id: track ids, bus ids, "master".
using MeterSnapshot = std::map<QString, NodeMeter>;

class AudioEngine {
public:
  AudioEngine(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources, RenderOptions opts = {});
  ~AudioEngine();
  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // Renders `count` frames starting at timeline sample `start` into `out` (2*count floats). Contiguous
  // calls (start == previous start + count) continue the effect state; anything else resets it first.
  // Not thread-safe; use one engine per thread (meters() and loudness are the exceptions).
  void render(std::int64_t start, std::int64_t count, float* out);
  // Forget effect state and position.
  void reset();

  MeterSnapshot meters() const;
  // Live loudness of the master output, from the moment it is enabled. Thread-safe.
  void enableLoudness(bool on);
  struct Loudness {
    double momentary = kSilenceLufs, shortTerm = kSilenceLufs, integrated = kSilenceLufs, truePeakDb = kSilenceLufs;
  };
  Loudness loudness() const;

  // Hosted plugin state (base64 for the mixer block), by insert id; refreshed on demand.
  std::map<QString, QString> pluginStates() const;

  struct Impl;

private:
  std::unique_ptr<Impl> d;
};

// Offline render for export and tests: deterministic, and thread-safe provided `sources` is (the
// providers in source.h are). Equivalent to a fresh engine rendering [start - preroll, start + count).
std::vector<float> renderRange(std::shared_ptr<const TimelineDoc> doc, std::shared_ptr<SourceProvider> sources, std::int64_t start,
                               std::int64_t count, const RenderOptions& opts = {});

// Timeline length in samples (end of the last item).
std::int64_t docDurationSamples(const TimelineDoc& doc);

// 16-bit PCM or 32-bit float WAV, interleaved stereo 48 kHz, for tests and debugging.
bool writeWav(const QString& path, const float* interleaved, std::int64_t frames, bool float32 = false);

} // namespace sf::audio
