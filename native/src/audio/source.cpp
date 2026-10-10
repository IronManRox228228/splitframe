#include "audio/source.h"

#include <algorithm>
#include <cstring>

namespace sf::audio {

FunctionSource::FunctionSource(std::int64_t totalSamples, Fn fn, double startSec) : fn_(std::move(fn)) {
  info_.sourceRate = AudioDecoder::kSampleRate;
  info_.sourceChannels = 2;
  info_.codec = QStringLiteral("function");
  info_.totalSamples = totalSamples;
  info_.durationSec = static_cast<double>(totalSamples) / AudioDecoder::kSampleRate;
  info_.startSec = startSec;
}

void FunctionSource::read(std::int64_t start, std::int64_t count, float* out) {
  std::memset(out, 0, static_cast<size_t>(count) * 2 * sizeof(float));
  const std::int64_t a = std::max<std::int64_t>(start, 0);
  const std::int64_t b = std::min(start + count, info_.totalSamples);
  if (b > a) fn_(a, b - a, out + (a - start) * 2);
}

MemorySource::MemorySource(std::vector<float> interleaved, double startSec) : data_(std::move(interleaved)) {
  info_.sourceRate = AudioDecoder::kSampleRate;
  info_.sourceChannels = 2;
  info_.codec = QStringLiteral("memory");
  info_.totalSamples = static_cast<std::int64_t>(data_.size() / 2);
  info_.durationSec = static_cast<double>(info_.totalSamples) / AudioDecoder::kSampleRate;
  info_.startSec = startSec;
}

void MemorySource::read(std::int64_t start, std::int64_t count, float* out) {
  std::memset(out, 0, static_cast<size_t>(count) * 2 * sizeof(float));
  const std::int64_t a = std::max<std::int64_t>(start, 0);
  const std::int64_t b = std::min(start + count, info_.totalSamples);
  if (b > a) std::memcpy(out + (a - start) * 2, data_.data() + a * 2, static_cast<size_t>(b - a) * 2 * sizeof(float));
}

DecoderSource::DecoderSource(std::unique_ptr<AudioDecoder> d) : dec_(std::move(d)), info_(dec_->info()) {}

std::shared_ptr<DecoderSource> DecoderSource::open(const QString& path, QString* error) {
  auto d = AudioDecoder::open(path, error);
  if (!d) return nullptr;
  return std::shared_ptr<DecoderSource>(new DecoderSource(std::move(d)));
}

void DecoderSource::read(std::int64_t start, std::int64_t count, float* out) {
  const std::lock_guard lock(m_);
  dec_->read(start, count, out);
}

void FileProvider::setFiles(std::map<QString, File> files) {
  const std::lock_guard lock(m_);
  // keep sources whose file did not change
  for (auto it = open_.begin(); it != open_.end();) {
    const auto f = files.find(it->first);
    const auto old = files_.find(it->first);
    if (f == files.end() || old == files_.end() || f->second.path != old->second.path) it = open_.erase(it);
    else ++it;
  }
  files_ = std::move(files);
}

std::shared_ptr<AudioSource> FileProvider::source(const QString& assetId) {
  const std::lock_guard lock(m_);
  const auto it = files_.find(assetId);
  if (it == files_.end() || !it->second.hasAudio) return nullptr;
  if (const auto o = open_.find(assetId); o != open_.end()) return o->second;
  auto s = DecoderSource::open(it->second.path);
  if (s) open_[assetId] = s;
  return s;
}

} // namespace sf::audio
