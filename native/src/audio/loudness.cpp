#include "audio/loudness.h"

#include <algorithm>
#include <cmath>

namespace sf::audio {

namespace {

constexpr int kSub = 4800;      // 100 ms at 48 kHz
constexpr int kTpHalf = 12;     // true-peak interpolator reaches 12 samples each side
constexpr double kPi = 3.14159265358979323846;

// BS.1770-4 K-weighting at 48 kHz: stage 1 high shelf, stage 2 RLB high-pass
struct Bq {
  double b0, b1, b2, a1, a2;
};
constexpr Bq kStage1{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585};
constexpr Bq kStage2{1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621};

double i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; ++k) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}

// 4 phases x 24 taps: windowed-sinc interpolation at fractions 0, 1/4, 1/2, 3/4 of a sample
struct TpFilter {
  double h[4][2 * kTpHalf];
  TpFilter() {
    const double beta = 9.0, norm = i0(beta);
    for (int p = 0; p < 4; ++p) {
      for (int j = 0; j < 2 * kTpHalf; ++j) {
        // tap j multiplies x[n - j]; the point being interpolated is x[n - kTpHalf] + p/4 of a sample
        const double d = static_cast<double>(kTpHalf - j) - static_cast<double>(p) / 4.0;
        const double sinc = std::fabs(d) < 1e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
        const double r = d / kTpHalf;
        const double w = std::fabs(r) >= 1 ? 0.0 : i0(beta * std::sqrt(1 - r * r)) / norm;
        h[p][j] = sinc * w;
      }
    }
  }
};
const TpFilter& tpFilter() {
  static const TpFilter f;
  return f;
}

double toLufs(double energy) { return energy > 0 ? -0.691 + 10.0 * std::log10(energy) : kSilenceLufs; }

} // namespace

LoudnessMeter::LoudnessMeter() = default;

void LoudnessMeter::reset() { *this = LoudnessMeter(); }

void LoudnessMeter::trueSample(int ch, float x) {
  Tp& t = tp_[ch];
  t.hist[t.pos] = x;
  const TpFilter& f = tpFilter();
  double best = 0;
  for (int p = 0; p < 4; ++p) {
    double y = 0;
    for (int j = 0; j < 2 * kTpHalf; ++j) y += static_cast<double>(t.hist[(t.pos - j + 24 * 2) % 24]) * f.h[p][j];
    best = std::max(best, std::fabs(y));
  }
  t.pos = (t.pos + 1) % 24;
  truePeak_ = std::max(truePeak_, best);
}

void LoudnessMeter::process(const float* in, std::int64_t frames) {
  for (std::int64_t i = 0; i < frames; ++i) {
    for (int ch = 0; ch < 2; ++ch) {
      const float x = in[i * 2 + ch];
      peak_ = std::max(peak_, static_cast<double>(std::fabs(x)));
      trueSample(ch, x);
      K& k = k_[ch];
      // stage 1, transposed direct form II
      double v = x;
      double y = kStage1.b0 * v + k.s1[0];
      k.s1[0] = kStage1.b1 * v - kStage1.a1 * y + k.s1[1];
      k.s1[1] = kStage1.b2 * v - kStage1.a2 * y;
      v = y;
      y = kStage2.b0 * v + k.s2[0];
      k.s2[0] = kStage2.b1 * v - kStage2.a1 * y + k.s2[1];
      k.s2[1] = kStage2.b2 * v - kStage2.a2 * y;
      subSum_[ch] += y * y;
    }
    if (++inSub_ == kSub) finishSubBlock();
  }
}

void LoudnessMeter::finishSubBlock() {
  subs_.push_back({subSum_[0], subSum_[1]});
  subSum_[0] = subSum_[1] = 0;
  inSub_ = 0;
  const size_t n = subs_.size();
  if (n >= 4) {
    double e = 0;
    for (size_t i = n - 4; i < n; ++i) e += subs_[i][0] + subs_[i][1];
    blockEnergy_.push_back(e / (4.0 * kSub));
  }
  if (n >= 30) {
    double e = 0;
    for (size_t i = n - 30; i < n; ++i) e += subs_[i][0] + subs_[i][1];
    shortEnergy_.push_back(e / (30.0 * kSub));
  }
}

double LoudnessMeter::momentary() const { return blockEnergy_.empty() ? kSilenceLufs : toLufs(blockEnergy_.back()); }
double LoudnessMeter::shortTerm() const { return shortEnergy_.empty() ? kSilenceLufs : toLufs(shortEnergy_.back()); }

double LoudnessMeter::integrated() const {
  double sum = 0;
  size_t n = 0;
  for (const double e : blockEnergy_)
    if (toLufs(e) > -70.0) {
      sum += e;
      ++n;
    }
  if (n == 0) return kSilenceLufs;
  const double relative = toLufs(sum / static_cast<double>(n)) - 10.0;
  sum = 0;
  n = 0;
  for (const double e : blockEnergy_) {
    const double l = toLufs(e);
    if (l > -70.0 && l > relative) {
      sum += e;
      ++n;
    }
  }
  return n == 0 ? kSilenceLufs : toLufs(sum / static_cast<double>(n));
}

double LoudnessMeter::loudnessRange() const {
  double sum = 0;
  size_t n = 0;
  for (const double e : shortEnergy_)
    if (toLufs(e) > -70.0) {
      sum += e;
      ++n;
    }
  if (n == 0) return 0;
  const double relative = toLufs(sum / static_cast<double>(n)) - 20.0;
  std::vector<double> v;
  for (const double e : shortEnergy_) {
    const double l = toLufs(e);
    if (l > -70.0 && l > relative) v.push_back(l);
  }
  if (v.size() < 2) return 0;
  std::sort(v.begin(), v.end());
  const auto pct = [&](double p) {
    const double pos = p * static_cast<double>(v.size() - 1);
    const size_t i = static_cast<size_t>(pos);
    const double f = pos - static_cast<double>(i);
    return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
  };
  return pct(0.95) - pct(0.10);
}

double LoudnessMeter::truePeakDb() const {
  const double tp = std::max(truePeak_, peak_);
  return tp > 0 ? 20.0 * std::log10(tp) : kSilenceLufs;
}
double LoudnessMeter::samplePeakDb() const { return peak_ > 0 ? 20.0 * std::log10(peak_) : kSilenceLufs; }

LoudnessResult measureLoudness(const float* interleaved, std::int64_t frames) {
  LoudnessMeter m;
  m.process(interleaved, frames);
  return {m.integrated(), m.loudnessRange(), m.truePeakDb(), m.samplePeakDb()};
}

double normalizeGainDb(const LoudnessResult& r, double target, std::optional<double> tpCeiling) {
  if (!std::isfinite(r.integrated)) return 0;
  double gain = target - r.integrated;
  if (tpCeiling && std::isfinite(r.truePeakDb)) gain = std::min(gain, *tpCeiling - r.truePeakDb);
  return gain;
}

} // namespace sf::audio
