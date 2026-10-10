#pragma once

// Fixtures for the audio suites: synthetic sources (nothing stored, so a two-hour file is free), docs
// with audio items, and a few measuring helpers.

#include "audio/effects.h"
#include "audio/engine.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "test_helpers.h"

#include <QTest>

#include <cmath>
#include <memory>
#include <vector>

namespace sf::test {

constexpr double kTwoPi = 6.283185307179586;

inline float sineAt(double freq, double amp, std::int64_t n, double phase = 0.0) {
  return static_cast<float>(amp * std::sin(kTwoPi * freq * static_cast<double>(n) / 48000.0 + phase));
}

// Same tone on both channels, `samples` long.
inline std::shared_ptr<audio::FunctionSource> sineSource(double freq, double amp, std::int64_t samples) {
  return std::make_shared<audio::FunctionSource>(samples, [=](std::int64_t start, std::int64_t count, float* out) {
    for (std::int64_t i = 0; i < count; ++i) out[i * 2] = out[i * 2 + 1] = sineAt(freq, amp, start + i);
  });
}

inline std::shared_ptr<audio::FunctionSource> dcSource(float v, std::int64_t samples) {
  return std::make_shared<audio::FunctionSource>(samples, [=](std::int64_t, std::int64_t count, float* out) {
    for (std::int64_t i = 0; i < count * 2; ++i) out[i] = v;
  });
}

// Deterministic noise in [-amp, amp], same on every read of a position.
inline float noiseAt(std::int64_t n, int channel, float amp) {
  std::uint64_t x = static_cast<std::uint64_t>(n) * 2 + static_cast<std::uint64_t>(channel);
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return amp * (static_cast<float>(x >> 40) / 8388608.0f - 1.0f);
}

inline std::shared_ptr<audio::FunctionSource> noiseSource(float amp, std::int64_t samples) {
  return std::make_shared<audio::FunctionSource>(samples, [=](std::int64_t start, std::int64_t count, float* out) {
    for (std::int64_t i = 0; i < count; ++i) {
      out[i * 2] = noiseAt(start + i, 0, amp);
      out[i * 2 + 1] = noiseAt(start + i, 1, amp);
    }
  });
}

// A project with the default text / video / audio tracks. Audio items go to the "ast_music" asset, video
// items to "ast_video" (test_helpers.h).
struct AudioFixture {
  TimelineDoc doc = makeDoc();
  std::shared_ptr<audio::MapProvider> sources = std::make_shared<audio::MapProvider>();

  std::vector<float> render(std::int64_t start, std::int64_t count, audio::RenderOptions opts = {}) const {
    return audio::renderRange(std::make_shared<const TimelineDoc>(doc), sources, start, count, opts);
  }
  void add(const Item& item) { doc = docWith(doc, {item}); }
};

// RMS of one channel over [from, to)
inline double rms(const std::vector<float>& v, std::int64_t from, std::int64_t to, int ch = 0) {
  double s = 0;
  for (std::int64_t i = from; i < to; ++i) s += static_cast<double>(v[static_cast<size_t>(i) * 2 + static_cast<size_t>(ch)]) * v[static_cast<size_t>(i) * 2 + static_cast<size_t>(ch)];
  return std::sqrt(s / static_cast<double>(to - from));
}

inline double peakOf(const std::vector<float>& v, std::int64_t from, std::int64_t to) {
  double p = 0;
  for (std::int64_t i = from * 2; i < to * 2; ++i) p = std::max(p, static_cast<double>(std::fabs(v[static_cast<size_t>(i)])));
  return p;
}

inline double toDb(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

// Interleaved stereo from a per-sample function
template <class F> std::vector<float> makeSignal(std::int64_t frames, F&& f) {
  std::vector<float> v(static_cast<size_t>(frames) * 2);
  for (std::int64_t i = 0; i < frames; ++i) {
    v[static_cast<size_t>(i) * 2] = f(i, 0);
    v[static_cast<size_t>(i) * 2 + 1] = f(i, 1);
  }
  return v;
}

// Runs a whole signal through an effect in 512-frame calls
inline std::vector<float> runEffect(audio::Effect& fx, std::vector<float> in, const std::vector<float>* sidechain = nullptr) {
  const std::int64_t frames = static_cast<std::int64_t>(in.size() / 2);
  for (std::int64_t at = 0; at < frames; at += 512) {
    const int n = static_cast<int>(std::min<std::int64_t>(512, frames - at));
    fx.process(in.data() + at * 2, n, sidechain ? sidechain->data() + at * 2 : nullptr);
  }
  return in;
}

} // namespace sf::test
