#pragma once

// Sound card output (miniaudio, WASAPI shared mode) and the audio master clock.
//
// The device callback pulls interleaved 48 kHz stereo float from a single-producer ring buffer that a
// render thread keeps filled. Underruns play silence and are counted. The callback also counts the
// frames it took from the ring ("content"): that counter, not the wall clock, is what AudioClockSource
// reports, so video that follows the PlaybackClock follows what the card actually consumed and stalls
// with it on an underrun. The card's own output latency (typically 10-30 ms in shared mode) is not
// subtracted.

#include "render/playback_clock.h"

#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sf::audio {

class AudioOutput {
public:
  AudioOutput();
  ~AudioOutput();
  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // Opens the default playback device and starts pulling (silence until something is written). False +
  // *error when there is no device. `ringFrames` is the ring capacity.
  bool start(QString* error = nullptr, int ringFrames = 24000);
  void stop();
  bool running() const { return running_.load(); }

  // Producer side (one thread). Returns frames accepted.
  int write(const float* interleaved, int frames);
  int freeFrames() const;
  int queuedFrames() const;
  // Drops everything queued and waits (briefly) until the device thread has noticed. The producer must
  // not write concurrently.
  void flush();

  // Frames the device has taken from the ring since start (silence from underruns excluded).
  std::int64_t contentFrames() const { return content_.load(std::memory_order_acquire); }
  // Frames the device asked for in total, silence included.
  std::int64_t deviceFrames() const { return device_.load(std::memory_order_acquire); }
  // Callbacks that found the ring empty while running.
  std::int64_t underruns() const { return underruns_.load(); }
  // Output gain applied in the callback (0 plays silence while the clock keeps running).
  void setGain(float g) { gain_.store(g); }
  int deviceSampleRate() const { return deviceRate_; }

  struct Impl;

private:
  void pull(float* out, int frames);

  std::unique_ptr<Impl> d;
  std::vector<float> ring_;
  int cap_ = 0;
  std::atomic<std::int64_t> head_{0}, tail_{0}; // frames, monotonically increasing
  std::atomic<int> flushEpoch_{0}, flushAck_{0};
  std::atomic<std::int64_t> content_{0}, device_{0}, underruns_{0};
  std::atomic<bool> running_{false};
  std::atomic<float> gain_{1.0f};
  int deviceRate_ = 0;
};

// A ClockSource for PlaybackClock. In audio mode time is the number of content frames the device has
// consumed; otherwise the system's steady clock. Switching modes keeps the reported time continuous, so
// it is safe even with the clock anchored.
class AudioClockSource final : public render::ClockSource {
public:
  explicit AudioClockSource(const AudioOutput* out = nullptr) : out_(out) {}
  void setOutput(const AudioOutput* out) {
    std::lock_guard<std::mutex> l(m_);
    out_ = out;
  }
  // audio = true makes the device the master (needs a running output).
  void setAudioMaster(bool audio);
  bool audioMaster() const { return audio_.load(); }
  std::int64_t nowNs() const override;

private:
  std::int64_t raw(bool audio) const;
  mutable std::mutex m_;
  const AudioOutput* out_;
  std::atomic<bool> audio_{false};
  std::atomic<std::int64_t> base_{0};
};

} // namespace sf::audio
