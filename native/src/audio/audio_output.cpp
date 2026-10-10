#include "audio/audio_output.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace sf::audio {

struct AudioOutput::Impl {
  ma_context context{};
  ma_device device{};
  bool haveContext = false;
  bool haveDevice = false;
};

AudioOutput::AudioOutput() : d(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { stop(); }

void AudioOutput::pull(float* out, int frames) {
  const int epoch = flushEpoch_.load(std::memory_order_acquire);
  if (epoch != flushAck_.load(std::memory_order_relaxed)) {
    tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    flushAck_.store(epoch, std::memory_order_release);
  }
  const std::int64_t head = head_.load(std::memory_order_acquire);
  const std::int64_t tail = tail_.load(std::memory_order_relaxed);
  const int avail = static_cast<int>(std::min<std::int64_t>(head - tail, frames));
  const float g = gain_.load(std::memory_order_relaxed);
  for (int i = 0; i < avail; ++i) {
    const size_t at = static_cast<size_t>((tail + i) % cap_) * 2;
    out[i * 2] = ring_[at] * g;
    out[i * 2 + 1] = ring_[at + 1] * g;
  }
  if (avail < frames) {
    std::memset(out + avail * 2, 0, static_cast<size_t>(frames - avail) * 2 * sizeof(float));
    if (avail == 0) underruns_.fetch_add(1, std::memory_order_relaxed);
  }
  tail_.store(tail + avail, std::memory_order_release);
  content_.fetch_add(avail, std::memory_order_acq_rel);
  device_.fetch_add(frames, std::memory_order_acq_rel);
}

bool AudioOutput::start(QString* error, int ringFrames) {
  if (running_) return true;
  auto fail = [&](const QString& m) {
    if (error) *error = m;
    stop();
    return false;
  };
  cap_ = std::max(4800, ringFrames);
  ring_.assign(static_cast<size_t>(cap_) * 2, 0.0f);
  head_ = 0;
  tail_ = 0;
  flushEpoch_ = 0;
  flushAck_ = 0;
  content_ = 0;
  device_ = 0;
  underruns_ = 0;

  ma_backend backends[] = {ma_backend_wasapi};
  ma_context_config cc = ma_context_config_init();
  if (ma_context_init(backends, 1, &cc, &d->context) != MA_SUCCESS) return fail(QStringLiteral("no audio backend"));
  d->haveContext = true;

  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = 48000;
  cfg.periodSizeInMilliseconds = 10;
  cfg.dataCallback = [](ma_device* dev, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioOutput*>(dev->pUserData)->pull(static_cast<float*>(out), static_cast<int>(frames));
  };
  cfg.pUserData = this;
  if (ma_device_init(&d->context, &cfg, &d->device) != MA_SUCCESS) return fail(QStringLiteral("no audio output device"));
  d->haveDevice = true;
  deviceRate_ = static_cast<int>(d->device.sampleRate);
  running_ = true;
  if (ma_device_start(&d->device) != MA_SUCCESS) {
    running_ = false;
    return fail(QStringLiteral("cannot start the audio device"));
  }
  return true;
}

void AudioOutput::stop() {
  running_ = false;
  if (d->haveDevice) {
    ma_device_uninit(&d->device);
    d->haveDevice = false;
  }
  if (d->haveContext) {
    ma_context_uninit(&d->context);
    d->haveContext = false;
  }
}

int AudioOutput::freeFrames() const {
  return cap_ - static_cast<int>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
}
int AudioOutput::queuedFrames() const {
  return static_cast<int>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
}

int AudioOutput::write(const float* in, int frames) {
  const int n = std::min(frames, freeFrames());
  const std::int64_t head = head_.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    const size_t at = static_cast<size_t>((head + i) % cap_) * 2;
    ring_[at] = in[i * 2];
    ring_[at + 1] = in[i * 2 + 1];
  }
  head_.store(head + n, std::memory_order_release);
  return n;
}

void AudioOutput::flush() {
  const int epoch = flushEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!running_) {
    tail_.store(head_.load());
    flushAck_.store(epoch);
    return;
  }
  for (int i = 0; i < 100 && flushAck_.load(std::memory_order_acquire) != epoch; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// ---------------------------------------------------------------- clock

std::int64_t AudioClockSource::raw(bool audio) const {
  if (audio && out_) return out_->contentFrames() * 1000000000LL / 48000;
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AudioClockSource::setAudioMaster(bool audio) {
  std::lock_guard<std::mutex> l(m_);
  if (audio == audio_.load()) return;
  if (audio && !out_) return;
  const std::int64_t before = raw(audio_.load()) + base_.load();
  audio_.store(audio);
  base_.store(before - raw(audio));
}

std::int64_t AudioClockSource::nowNs() const { return raw(audio_.load()) + base_.load(); }

} // namespace sf::audio
