#pragma once

// Audio for the mixer: decode + resample to 48 kHz, 32-bit float, interleaved stereo, whatever the
// source is (mono is duplicated at unity gain, surround is downmixed). One fixed format keeps the
// mixer, effects and waveform code free of conversions.
//
// Position model: output sample 0 is the stream's first sample and sample i sits at
// AudioStreamInfo::startSec + i / 48000 seconds from the container start, the same origin as
// VideoStreamInfo::startSec, so A/V alignment is plain addition. Seeks are sample accurate for
// sources that carry exact timestamps (PCM, AAC/mp4, FLAC, Opus...); constant-bitrate MP3 without a
// seek table can only be as exact as the demuxer's estimate.

#include <QString>

#include <memory>
#include <vector>

namespace sf {

struct AudioStreamInfo {
  int sourceRate = 0;
  int sourceChannels = 0;
  QString codec;
  qint64 totalSamples = 0;  // frames at 48 kHz, from the stream duration (exact for PCM, +-1 packet for lossy)
  double durationSec = 0;
  double startSec = 0;      // time of sample 0, seconds from container start
};

// NOT thread-safe (holds decoder state between reads so sequential reads don't re-seek); use it from
// one thread at a time.
class AudioDecoder {
public:
  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 2;

  // Reads headers; no decoding yet. Disk access: call from a worker.
  static std::unique_ptr<AudioDecoder> open(const QString& path, QString* error = nullptr);
  ~AudioDecoder();
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  const AudioStreamInfo& info() const;

  // Writes `count` stereo frames (2*count floats) starting at output sample `start` into `out`.
  // Samples before the stream start or past its end are silence. Returns how many frames came from
  // the stream (the rest are zeros). Sequential reads continue where the last one ended; anything
  // else seeks (to a bit before `start`, then decodes forward and discards, which is what makes it
  // sample accurate).
  qint64 read(qint64 start, qint64 count, float* out);
  std::vector<float> read(qint64 start, qint64 count);

private:
  AudioDecoder();
  struct Impl;
  std::unique_ptr<Impl> d;
};

} // namespace sf
