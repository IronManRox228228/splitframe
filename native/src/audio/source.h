#pragma once

// Where the mixer gets decoded audio from. An AudioSource is random access at 48 kHz stereo float
// (sample i at info().startSec + i / 48000 s, silence outside the stream) and safe to read from any
// thread; a SourceProvider maps asset ids to sources.

#include "media/audio_decoder.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace sf::audio {

class AudioSource {
public:
  virtual ~AudioSource() = default;
  virtual const AudioStreamInfo& info() const = 0;
  // Writes `count` stereo frames (2*count floats) starting at stream sample `start`; zeros outside the stream.
  // Thread-safe.
  virtual void read(std::int64_t start, std::int64_t count, float* out) = 0;
};

class SourceProvider {
public:
  virtual ~SourceProvider() = default;
  // null when the asset has no audio or cannot be opened (the item renders silent)
  virtual std::shared_ptr<AudioSource> source(const QString& assetId) = 0;
};

// A source computed on the fly, e.g. a test tone: nothing is stored, so a two-hour file costs no memory.
class FunctionSource final : public AudioSource {
public:
  using Fn = std::function<void(std::int64_t start, std::int64_t count, float* out)>;
  FunctionSource(std::int64_t totalSamples, Fn fn, double startSec = 0);
  const AudioStreamInfo& info() const override { return info_; }
  void read(std::int64_t start, std::int64_t count, float* out) override;

private:
  AudioStreamInfo info_;
  Fn fn_;
};

// Interleaved stereo samples held in memory.
class MemorySource final : public AudioSource {
public:
  explicit MemorySource(std::vector<float> interleaved, double startSec = 0);
  const AudioStreamInfo& info() const override { return info_; }
  void read(std::int64_t start, std::int64_t count, float* out) override;

private:
  AudioStreamInfo info_;
  std::vector<float> data_;
};

// Decoder-backed source. Reads are serialised: one decoder keeps its position between sequential reads,
// which is what playback does.
class DecoderSource final : public AudioSource {
public:
  static std::shared_ptr<DecoderSource> open(const QString& path, QString* error = nullptr);
  const AudioStreamInfo& info() const override { return info_; }
  void read(std::int64_t start, std::int64_t count, float* out) override;

private:
  explicit DecoderSource(std::unique_ptr<AudioDecoder> d);
  std::unique_ptr<AudioDecoder> dec_;
  AudioStreamInfo info_;
  std::mutex m_;
};

// Fixed table of in-memory/function sources (tests, tools).
class MapProvider final : public SourceProvider {
public:
  void add(const QString& assetId, std::shared_ptr<AudioSource> s) { map_[assetId] = std::move(s); }
  std::shared_ptr<AudioSource> source(const QString& assetId) override {
    const auto it = map_.find(assetId);
    return it == map_.end() ? nullptr : it->second;
  }

private:
  std::map<QString, std::shared_ptr<AudioSource>> map_;
};

// Files on disk, opened on first use and kept.
class FileProvider final : public SourceProvider {
public:
  struct File {
    QString path;
    bool hasAudio = true;
  };
  FileProvider() = default;
  explicit FileProvider(std::map<QString, File> files) : files_(std::move(files)) {}
  void setFiles(std::map<QString, File> files);
  std::shared_ptr<AudioSource> source(const QString& assetId) override;

private:
  std::mutex m_;
  std::map<QString, File> files_;
  std::map<QString, std::shared_ptr<AudioSource>> open_;
};

} // namespace sf::audio
