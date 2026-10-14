#include "audio/audio_analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace sf::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();

double rmsToDb(double meanSquare) {
  const double rms = std::sqrt(meanSquare);
  return rms <= 1e-5 ? -100.0 : 20.0 * std::log10(rms);
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return -100.0;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
  return v[i];
}

} // namespace

// ---------------------------------------------------------------- MixAnalyzer

void MixAnalyzer::process(const float* in, std::int64_t frames) {
  constexpr std::int64_t kRunGap = 2400; // 50 ms
  while (frames > 0) {
    const std::int64_t n = std::min(frames, static_cast<std::int64_t>(kAnalysisRate) - inSecond_);
    meter_.process(in, n);
    for (std::int64_t i = 0; i < n; ++i) {
      const float l = in[i * 2], r = in[i * 2 + 1];
      blockSum_ += static_cast<double>(l) * l + static_cast<double>(r) * r;
      if (++inBlock_ == kGateBlock) {
        blockDb_.push_back(rmsToDb(blockSum_ / (2.0 * kGateBlock)));
        blockSum_ = 0;
        inBlock_ = 0;
      }
      const int c = (std::fabs(l) >= kClipLevel ? 1 : 0) + (std::fabs(r) >= kClipLevel ? 1 : 0);
      if (c > 0) {
        const std::int64_t f = frames_ + i;
        clipped_ += c;
        if (f - lastClipFrame_ <= kRunGap && keepCur_) {
          runs_.back().endSample = f;
          runs_.back().samples += c;
        } else if (f - lastClipFrame_ > kRunGap) {
          keepCur_ = runs_.size() < kMaxClipRuns;
          if (keepCur_) runs_.push_back({f, f, c});
        }
        lastClipFrame_ = f;
      }
    }
    in += n * 2;
    frames -= n;
    frames_ += n;
    inSecond_ += n;
    if (inSecond_ == kAnalysisRate) {
      shortTerm_.push_back(meter_.shortTerm());
      inSecond_ = 0;
    }
  }
}

MixMeasurement MixAnalyzer::result() const {
  MixMeasurement m;
  m.loudness = {meter_.integrated(), meter_.loudnessRange(), meter_.truePeakDb(), meter_.samplePeakDb()};
  m.noiseFloorDb = blockDb_.empty() ? -100.0 : percentile(blockDb_, 0.10);
  m.clippedSamples = clipped_;
  m.clipRuns = runs_;
  m.shortTermLufs = shortTerm_;
  m.frames = frames_;
  return m;
}

MixMeasurement measureMix(const float* interleaved, std::int64_t frames) {
  MixAnalyzer a;
  a.process(interleaved, frames);
  return a.result();
}

// ---------------------------------------------------------------- ranges

std::vector<Span> mergeSpans(std::vector<Span> spans, std::int64_t minGap) {
  spans.erase(std::remove_if(spans.begin(), spans.end(), [](const Span& s) { return s.end <= s.start; }), spans.end());
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.start < b.start || (a.start == b.start && a.end < b.end); });
  std::vector<Span> out;
  for (const Span& s : spans) {
    if (!out.empty() && s.start - out.back().end < std::max<std::int64_t>(minGap, 1)) out.back().end = std::max(out.back().end, s.end);
    else out.push_back(s);
  }
  return out;
}

void EnergyGate::process(const float* in, std::int64_t frames) {
  for (std::int64_t i = 0; i < frames; ++i) {
    const double l = static_cast<double>(in[i * 2]), r = static_cast<double>(in[i * 2 + 1]);
    blockSum_ += l * l + r * r;
    if (++inBlock_ == kGateBlock) {
      blockDb_.push_back(rmsToDb(blockSum_ / (2.0 * kGateBlock)));
      blockSum_ = 0;
      inBlock_ = 0;
    }
  }
}

double EnergyGate::thresholdDb(double floorDb) const {
  if (blockDb_.empty()) return floorDb;
  const double lo = percentile(blockDb_, 0.10), hi = percentile(blockDb_, 0.95);
  // clear gap between a quiet floor and loud passages: gate in the middle; a steady signal: just the fixed floor
  return hi - lo < 12.0 ? floorDb : std::max(floorDb, 0.5 * (lo + hi));
}

std::vector<Span> EnergyGate::ranges(double minSpeechMs, double floorDb) const {
  const double thr = thresholdDb(floorDb);
  const std::int64_t minBlocks = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(minSpeechMs / 50.0)));
  std::vector<Span> out;
  std::int64_t run = -1;
  const std::int64_t n = static_cast<std::int64_t>(blockDb_.size());
  for (std::int64_t b = 0; b <= n; ++b) {
    const bool on = b < n && blockDb_[static_cast<size_t>(b)] >= thr && blockDb_[static_cast<size_t>(b)] > -100.0;
    if (on && run < 0) run = b;
    if (!on && run >= 0) {
      if (b - run >= minBlocks) out.push_back({run * kGateBlock, b * kGateBlock});
      run = -1;
    }
  }
  return out;
}

void EnergyMeter::process(const float* in, std::int64_t frames) {
  for (std::int64_t i = 0; i < frames * 2; ++i) sum_ += static_cast<double>(in[i]) * in[i];
  frames_ += frames;
}

double EnergyMeter::db() const {
  if (frames_ <= 0 || sum_ <= 0) return -kInf;
  return 10.0 * std::log10(sum_ / (2.0 * static_cast<double>(frames_)));
}

double marginDb(double speechDb, double musicDb) {
  if (!std::isfinite(speechDb)) return -kInf;
  if (!std::isfinite(musicDb)) return kInf;
  return speechDb - musicDb;
}

// ---------------------------------------------------------------- ducking

std::vector<DuckKey> duckKeyframes(const std::vector<Span>& ranges, const DuckParams& p, double fps) {
  std::vector<DuckKey> keys;
  if (ranges.empty() || p.depthDb <= 0) return keys;
  const std::int64_t aF = std::max<std::int64_t>(1, std::llround(p.attackMs * fps / 1000.0));
  const std::int64_t rF = std::max<std::int64_t>(1, std::llround(p.releaseMs * fps / 1000.0));
  const double low = p.baseGain * std::pow(10.0, -p.depthDb / 20.0);
  const auto push = [&](std::int64_t frame, double v) {
    if (!keys.empty() && frame <= keys.back().frame) {
      if (frame == keys.back().frame) keys.back().value = std::min(keys.back().value, v); // same frame: the lower wins
      return;
    }
    keys.push_back({frame, v});
  };
  for (size_t i = 0; i < ranges.size(); ++i) {
    const Span& s = ranges[i];
    const std::int64_t down = std::max<std::int64_t>(0, s.start - aF);
    const bool bridged = i > 0 && down <= ranges[i - 1].end + rF; // the previous range never came back up
    if (!bridged) {
      if (down < s.start) push(down, p.baseGain);
      push(s.start, low);
    }
    const bool bridgesNext = i + 1 < ranges.size() && std::max<std::int64_t>(0, ranges[i + 1].start - aF) <= s.end + rF;
    if (!bridgesNext) {
      push(s.end, low);
      push(s.end + rF, p.baseGain);
    }
  }
  return keys;
}

// ---------------------------------------------------------------- music structure

namespace {

constexpr int kWin = 512;
constexpr double kEnvRate = static_cast<double>(kAnalysisRate) / kOnsetHop; // 200 Hz
constexpr std::int64_t kMaxAnalysisFrames = static_cast<std::int64_t>(kAnalysisRate) * 600;

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

double envTime(double idx) { return (idx * kOnsetHop + kWin / 2.0) / kAnalysisRate; }

double interp(const std::vector<float>& e, double pos) {
  if (pos < 0 || pos >= static_cast<double>(e.size()) - 1) return 0;
  const size_t i = static_cast<size_t>(pos);
  const double f = pos - static_cast<double>(i);
  return static_cast<double>(e[i]) * (1 - f) + static_cast<double>(e[i + 1]) * f;
}

} // namespace

std::vector<float> onsetEnvelope(const float* in, std::int64_t frames) {
  std::vector<float> env;
  if (frames < kWin + kOnsetHop) return env;
  const std::int64_t n = (frames - kWin) / kOnsetHop + 1;
  static const std::vector<double> win = [] {
    std::vector<double> w(kWin);
    for (int i = 0; i < kWin; ++i) w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kWin);
    return w;
  }();
  std::vector<std::complex<double>> buf(kWin);
  std::vector<double> prev(kWin / 2, 0.0), cur(kWin / 2, 0.0);
  std::vector<float> flux(static_cast<size_t>(n), 0.0f);
  for (std::int64_t f = 0; f < n; ++f) {
    const float* p = in + f * kOnsetHop * 2;
    for (int i = 0; i < kWin; ++i) buf[static_cast<size_t>(i)] = 0.5 * (static_cast<double>(p[i * 2]) + static_cast<double>(p[i * 2 + 1])) * win[static_cast<size_t>(i)];
    fft(buf);
    double sum = 0;
    for (int k = 1; k < kWin / 2; ++k) {
      cur[static_cast<size_t>(k)] = std::log1p(1000.0 * std::abs(buf[static_cast<size_t>(k)]) / (kWin / 4.0));
      if (f > 0) sum += std::max(0.0, cur[static_cast<size_t>(k)] - prev[static_cast<size_t>(k)]);
    }
    flux[static_cast<size_t>(f)] = static_cast<float>(sum);
    std::swap(prev, cur);
  }
  // detrend with a 1 s moving average, keep what rises above it
  const std::int64_t half = static_cast<std::int64_t>(kEnvRate / 2);
  std::vector<double> pre(static_cast<size_t>(n) + 1, 0.0);
  for (std::int64_t i = 0; i < n; ++i) pre[static_cast<size_t>(i) + 1] = pre[static_cast<size_t>(i)] + static_cast<double>(flux[static_cast<size_t>(i)]);
  env.resize(static_cast<size_t>(n));
  for (std::int64_t i = 0; i < n; ++i) {
    const std::int64_t a = std::max<std::int64_t>(0, i - half), b = std::min<std::int64_t>(n, i + half + 1);
    const double avg = (pre[static_cast<size_t>(b)] - pre[static_cast<size_t>(a)]) / static_cast<double>(b - a);
    env[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, static_cast<double>(flux[static_cast<size_t>(i)]) - avg));
  }
  return env;
}

namespace {

// 4th-order Butterworth-ish low-pass (two RBJ biquads, Q 0.707) on the mono mix, for the kick / sub band
struct LowPass {
  double b0, b1, b2, a1, a2;
  double z1[2] = {0, 0}, z2[2] = {0, 0};
  explicit LowPass(double fc) {
    const double w = 2.0 * kPi * fc / kAnalysisRate, c = std::cos(w), al = std::sin(w) / (2.0 * 0.7071);
    const double a0 = 1 + al;
    b0 = (1 - c) / 2 / a0;
    b1 = (1 - c) / a0;
    b2 = b0;
    a1 = -2 * c / a0;
    a2 = (1 - al) / a0;
  }
  double run(int s, double x) {
    const double y = b0 * x + z1[s];
    z1[s] = b1 * x - a1 * y + z2[s];
    z2[s] = b2 * x - a2 * y;
    return y;
  }
  double tick(double x) { return run(1, run(0, x)); }
};
constexpr double kLowHz = 150.0;

double monoAt(const float* in, std::int64_t i) { return 0.5 * (static_cast<double>(in[i * 2]) + static_cast<double>(in[i * 2 + 1])); }

// Onset envelope of the low band only (kick / sub): rise in dB of the 10 ms energy of the low-passed signal, detrended.
// Same frame grid as onsetEnvelope (kOnsetHop), n frames.
std::vector<float> lowOnsetEnvelope(const float* in, std::int64_t frames, size_t n) {
  std::vector<float> flux(n, 0.0f);
  LowPass lp(kLowHz);
  // energy per hop block, then each frame spans two blocks (480 samples)
  std::vector<double> e(n + 1, 0.0);
  const std::int64_t total = std::min<std::int64_t>(frames, static_cast<std::int64_t>(n + 1) * kOnsetHop);
  for (std::int64_t i = 0; i < total; ++i) {
    const double y = lp.tick(monoAt(in, i));
    e[static_cast<size_t>(i / kOnsetHop)] += y * y;
  }
  double prev = -100;
  for (size_t f = 0; f < n; ++f) {
    const double db = 10.0 * std::log10((e[f] + e[f + 1]) / (2.0 * kOnsetHop) + 1e-10);
    flux[f] = static_cast<float>(f > 0 ? std::max(0.0, db - prev) : 0.0);
    prev = db;
  }
  const std::int64_t half = static_cast<std::int64_t>(kEnvRate / 2);
  const std::int64_t nn = static_cast<std::int64_t>(n);
  std::vector<double> pre(n + 1, 0.0);
  for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + static_cast<double>(flux[i]);
  std::vector<float> env(n);
  for (std::int64_t i = 0; i < nn; ++i) {
    const std::int64_t a = std::max<std::int64_t>(0, i - half), b = std::min<std::int64_t>(nn, i + half + 1);
    const double avg = (pre[static_cast<size_t>(b)] - pre[static_cast<size_t>(a)]) / static_cast<double>(b - a);
    env[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, static_cast<double>(flux[static_cast<size_t>(i)]) - avg));
  }
  return env;
}

// |mean(a over the next w) - mean(a over the previous w)|, the larger of the two bands, at each index. Near either end the window
// shrinks to what is there (never under `minW`), so a change in the first or last few beats is still seen; 0 where even that is missing.
std::vector<double> noveltyCurve(const std::vector<double>& a, const std::vector<double>& b, int w, int minW) {
  const int n = static_cast<int>(a.size());
  std::vector<double> d(static_cast<size_t>(n) + 2, 0.0);
  const auto mean = [&](const std::vector<double>& v, int x, int y) {
    double s = 0;
    for (int i = x; i < y; ++i) s += v[static_cast<size_t>(i)];
    return s / (y - x);
  };
  for (int i = minW; i + minW <= n; ++i) {
    const int we = std::min({w, i, n - i});
    d[static_cast<size_t>(i)] = std::max(std::fabs(mean(a, i, i + we) - mean(a, i - we, i)), std::fabs(mean(b, i, i + we) - mean(b, i - we, i)));
  }
  return d;
}

// local maxima of d at or above `thr`, strongest first, at least `sep` apart
std::vector<std::pair<double, int>> pickPeaks(const std::vector<double>& d, int sep, double thr) {
  std::vector<std::pair<double, int>> cand, out;
  for (size_t i = 1; i + 1 < d.size(); ++i)
    if (d[i] >= thr && d[i] >= d[i - 1] && d[i] > d[i + 1]) cand.push_back({d[i], static_cast<int>(i)});
  std::sort(cand.begin(), cand.end(), [](const auto& x, const auto& y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
  for (const auto& c : cand) {
    bool free = true;
    for (const auto& o : out)
      if (std::abs(o.second - c.second) < sep) free = false;
    if (free) out.push_back(c);
  }
  return out;
}

} // namespace

MusicStructure analyseMusic(const float* in, std::int64_t frames) {
  MusicStructure ms;
  frames = std::min(frames, kMaxAnalysisFrames);
  ms.durationSec = static_cast<double>(frames) / kAnalysisRate;
  std::vector<float> env = onsetEnvelope(in, frames);
  if (env.size() < static_cast<size_t>(6 * kEnvRate)) {
    ms.note = "Too short to find a tempo (needs about 6 seconds).";
    return ms;
  }
  double r0 = 0, peak = 0;
  for (float v : env) {
    r0 += static_cast<double>(v) * v;
    peak = std::max(peak, static_cast<double>(v));
  }
  if (peak < 1.0) { // a steady tone flickers around 0.1; the quietest hit worth calling an onset is far above 1
    ms.note = "No onsets: the audio is silent or perfectly steady.";
    return ms;
  }
  const std::int64_t ne = static_cast<std::int64_t>(env.size());
  r0 /= static_cast<double>(ne);

  // final hit: the last onset peak of at least 20% of the strongest one, so the decaying tail (small peaks) is not taken for it
  for (std::int64_t i = ne - 2; i >= 1; --i) {
    const float v = env[static_cast<size_t>(i)];
    if (v >= 0.2 * peak && v >= env[static_cast<size_t>(i - 1)] && v > env[static_cast<size_t>(i + 1)]) {
      ms.finalHitSec = envTime(static_cast<double>(i));
      break;
    }
  }

  // coarse tempo: autocorrelation of the onset envelope, 60..200 BPM, octave chosen with a prior around 120 BPM
  const std::int64_t lagMin = static_cast<std::int64_t>(std::llround(60.0 * kEnvRate / 200.0)), lagMax = static_cast<std::int64_t>(std::llround(60.0 * kEnvRate / 60.0));
  const std::int64_t lagTop = std::min<std::int64_t>(2 * lagMax, ne / 2);
  std::vector<double> r(static_cast<size_t>(lagTop) + 1, 0.0);
  for (std::int64_t lag = lagMin; lag <= lagTop; ++lag) {
    double s = 0;
    for (std::int64_t i = 0; i + lag < ne; ++i) s += static_cast<double>(env[static_cast<size_t>(i)]) * static_cast<double>(env[static_cast<size_t>(i + lag)]);
    r[static_cast<size_t>(lag)] = s / static_cast<double>(ne - lag) / r0;
  }
  double bestScore = -1, bestLag = 0;
  for (std::int64_t lag = lagMin; lag <= std::min(lagMax, lagTop); ++lag) {
    const double bpm = 60.0 * kEnvRate / static_cast<double>(lag);
    const double oct = std::log2(bpm / 120.0) / 0.7;
    const double prior = std::exp(-0.5 * oct * oct);
    double s = r[static_cast<size_t>(lag)];
    if (2 * lag <= lagTop) s += 0.5 * r[static_cast<size_t>(2 * lag)];
    s *= prior;
    if (s > bestScore) {
      bestScore = s;
      bestLag = static_cast<double>(lag);
    }
  }
  const double coarseBpm = 60.0 * kEnvRate / bestLag;
  ms.tempoConfidence = std::clamp(r[static_cast<size_t>(bestLag)], 0.0, 1.0);

  // fine tempo and phase: a comb of beats laid over the (lightly smoothed) envelope, best BPM and offset win
  std::vector<float> sm(env.size());
  for (size_t i = 0; i < env.size(); ++i) sm[i] = 0.25f * env[i > 0 ? i - 1 : i] + 0.5f * env[i] + 0.25f * env[i + 1 < env.size() ? i + 1 : i];
  double bestComb = -1, bpm = coarseBpm, phase = 0;
  for (double cand = coarseBpm * 0.96; cand <= coarseBpm * 1.04 + 1e-9; cand += 0.05) {
    const double period = 60.0 * kEnvRate / cand;
    for (double ph = 0; ph < period; ph += 0.5) {
      double s = 0;
      int cnt = 0;
      for (double pos = ph; pos < static_cast<double>(ne) - 1; pos += period, ++cnt) s += interp(sm, pos);
      if (cnt == 0) continue;
      s /= cnt;
      if (s > bestComb) {
        bestComb = s;
        bpm = cand;
        phase = ph;
      }
    }
  }
  ms.bpm = bpm;
  ms.beatPeriodSec = 60.0 / bpm;
  const double periodF = 60.0 * kEnvRate / bpm;
  std::vector<double> beatIdx;
  for (double pos = phase; envTime(pos) < ms.durationSec; pos += periodF) beatIdx.push_back(pos);
  for (double pos : beatIdx) ms.beats.push_back(envTime(pos));
  if (ms.beats.size() < 4) {
    ms.note = "Fewer than four beats found.";
    return ms;
  }

  // Per-beat level in two bands (full, and low < 150 Hz) as mean-square energy; a beat spans one grid period from its time.
  const size_t nbeats = ms.beats.size();
  std::vector<double> bf(nbeats, 0.0), bl(nbeats, 0.0);
  {
    LowPass lp(kLowHz);
    std::vector<std::int64_t> start(nbeats + 1);
    for (size_t k = 0; k < nbeats; ++k) start[k] = static_cast<std::int64_t>(ms.beats[k] * kAnalysisRate);
    start[nbeats] = start[nbeats - 1] + static_cast<std::int64_t>(ms.beatPeriodSec * kAnalysisRate);
    size_t k = 0;
    std::vector<double> cf(nbeats, 0.0), cl(nbeats, 0.0);
    std::vector<std::int64_t> cnt(nbeats, 0);
    for (std::int64_t i = 0; i < frames; ++i) {
      const double x = monoAt(in, i), y = lp.tick(x);
      if (i < start[0]) continue;
      while (k < nbeats && i >= start[k + 1]) ++k;
      if (k >= nbeats) break;
      cf[k] += x * x;
      cl[k] += y * y;
      ++cnt[k];
    }
    for (size_t j = 0; j < nbeats; ++j)
      if (cnt[j] > 0) {
        bf[j] = cf[j] / static_cast<double>(cnt[j]);
        bl[j] = cl[j] / static_cast<double>(cnt[j]);
      }
  }
  const auto dbOf = [](double ms2) { return ms2 > 1e-10 ? 10.0 * std::log10(ms2) : -100.0; };
  std::vector<double> bfDb(nbeats), blDb(nbeats);
  for (size_t j = 0; j < nbeats; ++j) {
    bfDb[j] = dbOf(bf[j]);
    blDb[j] = dbOf(bl[j]);
  }

  // downbeat (4/4): of the four possible bar phases, score each by (1) how much low-band onset (kick) its beats carry, falling back to
  // the full-band onset when there is no low band, and (2) how many section changes (level change points in either band) fall on its bar lines
  const std::vector<float> lowEnv = lowOnsetEnvelope(in, frames, env.size());
  float lowPeak = 0;
  for (float v : lowEnv) lowPeak = std::max(lowPeak, v);
  const bool useLow = lowPeak >= 6.0f;
  const std::vector<float>& weigh = useLow ? lowEnv : sm;
  double kick[4] = {0, 0, 0, 0};
  int count[4] = {0, 0, 0, 0};
  for (size_t k = 0; k < beatIdx.size(); ++k) {
    const std::int64_t c = static_cast<std::int64_t>(std::llround(beatIdx[k]));
    float m = 0;
    for (std::int64_t i = std::max<std::int64_t>(0, c - 4); i <= std::min<std::int64_t>(ne - 1, c + 4); ++i) m = std::max(m, weigh[static_cast<size_t>(i)]);
    kick[k % 4] += static_cast<double>(m);
    ++count[k % 4];
  }
  double kickMean[4], kickSum = 0;
  for (int p = 0; p < 4; ++p) {
    kickMean[p] = count[p] > 0 ? kick[p] / count[p] : 0;
    kickSum += kickMean[p];
  }
  if (useLow) { // blend with the low-band level of each beat (the dB rise of an onset compresses a loud kick against a soft one)
    double lvl[4] = {0, 0, 0, 0}, lvlSum = 0;
    for (size_t k = 0; k < nbeats; ++k) lvl[k % 4] += std::sqrt(bl[k]);
    for (int p = 0; p < 4; ++p) lvlSum += lvl[p];
    if (lvlSum > 0 && kickSum > 0)
      for (int p = 0; p < 4; ++p) kickMean[p] = 0.5 * kickMean[p] / kickSum + 0.5 * lvl[p] / lvlSum;
    kickSum = 1.0;
  }
  // Section changes, in two steps. Coarse: level change over a long window (about 4 bars) in either band, 3 dB or more, which ignores
  // fills and one-bar dropouts. Fine: within 2 beats of each coarse change the beat with the largest contrast of the 4 beats after vs the 4 before.
  // A final beat that runs past the end of the audio is left out, its level is not a musical change.
  size_t nComplete = nbeats;
  while (nComplete > 0 && static_cast<std::int64_t>((ms.beats[nComplete - 1] + ms.beatPeriodSec) * kAnalysisRate) > frames) --nComplete;
  const std::vector<double> bfC(bfDb.begin(), bfDb.begin() + static_cast<std::ptrdiff_t>(nComplete)), blC(blDb.begin(), blDb.begin() + static_cast<std::ptrdiff_t>(nComplete));
  const int wBeat = std::clamp(static_cast<int>(nbeats) / 8, 4, 16);
  const std::vector<double> coarse = noveltyCurve(bfC, blC, wBeat, 4), dn = noveltyCurve(bfC, blC, 4, 4);
  struct Change {
    int beat;
    double v;
  };
  std::vector<Change> changes;
  for (const auto& [v, b0] : pickPeaks(coarse, wBeat, 3.0)) {
    int at = b0;
    double bestD = -1;
    for (int i = std::max(4, b0 - 2); i <= std::min(static_cast<int>(dn.size()) - 1, b0 + 2); ++i)
      if (dn[static_cast<size_t>(i)] > bestD + 1e-9 || (std::fabs(dn[static_cast<size_t>(i)] - bestD) <= 1e-9 && std::abs(i - b0) < std::abs(at - b0))) {
        bestD = dn[static_cast<size_t>(i)];
        at = i;
      }
    changes.push_back({at, v});
  }
  double vote[4] = {0, 0, 0, 0}, voteSum = 0;
  for (const Change& c : changes) {
    vote[c.beat % 4] += c.v;
    voteSum += c.v;
  }
  // kick share decides; boundary votes only break a near-tie (best kick share under 1.15x the runner-up)
  int kBest = 0;
  for (int p = 1; p < 4; ++p)
    if (kickMean[p] > kickMean[kBest]) kBest = p;
  double kSecond = 0;
  for (int p = 0; p < 4; ++p)
    if (p != kBest) kSecond = std::max(kSecond, kickMean[p]);
  const double kRatio = kickSum <= 0 ? 1.0 : (kSecond > 0 ? kickMean[kBest] / kSecond : 1e9);
  const bool nearTie = kRatio < 1.15;
  int best = kBest;
  if (nearTie) {
    double phaseScore = -1;
    for (int p = 0; p < 4; ++p) {
      const double sc = (kickSum > 0 ? kickMean[p] / kickSum : 0.25) + (voteSum > 0 ? vote[p] / voteSum : 0.0);
      if (sc > phaseScore * 1.02 + 1e-12) { // ties keep the earlier phase
        phaseScore = sc;
        best = p;
      }
    }
  }
  ms.downbeatPhase = best;
  {
    std::string ev = std::string(useLow ? "low-band kick" : "full-band onset (no low band)") + " share per phase [";
    for (int p = 0; p < 4; ++p) ev += (p ? ", " : "") + std::to_string(kickSum > 0 ? kickMean[p] / kickSum : 0.25).substr(0, 4);
    ev += "]; section changes on bar lines per phase [";
    for (int p = 0; p < 4; ++p) ev += (p ? ", " : "") + std::to_string(voteSum > 0 ? vote[p] / voteSum : 0.0).substr(0, 4);
    ev += "]; ";
    ev += nearTie ? "kick shares near-tied (best/second " + std::to_string(std::min(kRatio, 99.0)).substr(0, 4) + " < 1.15), section changes decided: phase " + std::to_string(best) + " won"
                  : "kick share decided (best/second " + std::to_string(std::min(kRatio, 99.0)).substr(0, 4) + " >= 1.15): phase " + std::to_string(best) + " won";
    ms.downbeatEvidence = ev;
  }
  for (size_t k = static_cast<size_t>(best); k < nbeats; k += 4) ms.downbeats.push_back(ms.beats[k]);

  // bars: four beats from each downbeat (a final partial bar needs two), level in both bands
  struct Bar {
    double start, full, low;
  };
  std::vector<Bar> bars;
  std::vector<double> barF, barL;
  for (size_t b = 0; b < ms.downbeats.size(); ++b) {
    const size_t k0 = static_cast<size_t>(best) + b * 4;
    const size_t k1 = std::min(nbeats, k0 + 4);
    if (k1 - k0 < (k0 + 4 <= nbeats ? 4u : 2u)) continue;
    double f = 0, l = 0;
    for (size_t k = k0; k < k1; ++k) {
      f += bf[k];
      l += bl[k];
    }
    f /= static_cast<double>(k1 - k0);
    l /= static_cast<double>(k1 - k0);
    bars.push_back({ms.downbeats[b], dbOf(f), dbOf(l)});
    barF.push_back(bars.back().full);
    barL.push_back(bars.back().low);
  }
  ms.valid = true;
  const int nb = static_cast<int>(bars.size());
  if (nb == 0) return ms;

  // sections: each change is snapped to the nearest downbeat. Music often opens with a pickup, so the 8-bar (else 4-bar) phrase grid is
  // anchored on the first strong boundary, not on bar 0, and a boundary only moves onto it when its evidence is within 80%.
  struct Cand {
    int bar;
    double v;
  };
  std::vector<Cand> cands;
  double maxV = 0;
  for (const Change& c : changes) {
    const int bar = static_cast<int>(std::lround((c.beat - best) / 4.0));
    if (bar < 1 || bar >= nb) continue;
    cands.push_back({bar, c.v});
    maxV = std::max(maxV, c.v);
  }
  int anchor = -1;
  for (const Cand& c : cands)
    if (c.v >= 0.4 * maxV && (anchor < 0 || c.bar < anchor)) anchor = c.bar;
  const auto novAt = [&](int bar) {
    const size_t i = static_cast<size_t>(best) + static_cast<size_t>(bar) * 4;
    return i < dn.size() ? dn[i] : 0.0;
  };
  std::vector<int> cuts{0};
  std::vector<double> cutV{0.0}; // boundary strength per cut
  for (const Cand& cd : cands) {
    int b = cd.bar;
    for (const int mult : {8, 4}) {
      int pick = -1;
      double pickV = 0;
      for (int c = cd.bar - 1; c <= cd.bar + 1; ++c)
        if (c >= 1 && c < nb && ((c - anchor) % mult + mult) % mult == 0 && novAt(c) >= 0.8 * cd.v && novAt(c) > pickV) {
          pick = c;
          pickV = novAt(c);
        }
      if (pick >= 0) {
        b = pick;
        break;
      }
    }
    bool free = true;
    for (int c : cuts)
      if (std::abs(c - b) < (c == 0 ? 1 : 2)) free = false;
    if (free) {
      cuts.push_back(b);
      cutV.push_back(cd.v);
    }
  }
  // Weak boundaries merge: an interior section under 8 bars (a phrase) is not a section, so the weaker of its two bounding cuts goes,
  // unless that cut is itself strong (at least half the strongest boundary), which keeps real short pickups and tails.
  constexpr int kMinSectionBars = 8;
  for (bool again = true; again;) {
    again = false;
    std::vector<size_t> order(cuts.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return cuts[x] < cuts[y]; });
    for (size_t k = 1; k + 1 < order.size() && !again; ++k) {
      const size_t a = order[k], b = order[k + 1];
      if (cuts[b] - cuts[a] >= kMinSectionBars) continue;
      const size_t weak = cutV[a] <= cutV[b] ? a : b;
      if (cutV[weak] >= 0.5 * maxV) continue;
      cuts.erase(cuts.begin() + static_cast<std::ptrdiff_t>(weak));
      cutV.erase(cutV.begin() + static_cast<std::ptrdiff_t>(weak));
      again = true;
    }
  }
  std::sort(cuts.begin(), cuts.end());
  // the tail after the final hit: when the last cut sits a bar or two past a hit that lands on a bar line, the hit and its decay are the outro
  if (ms.finalHitSec >= 0 && cuts.size() >= 2) {
    const int hitBar = static_cast<int>(std::lround((ms.finalHitSec - bars.front().start) / (4.0 * ms.beatPeriodSec)));
    const double off = ms.finalHitSec - bars[static_cast<size_t>(std::clamp(hitBar, 0, nb - 1))].start;
    if (hitBar > cuts[cuts.size() - 2] && hitBar < cuts.back() && cuts.back() - hitBar <= 2 && std::fabs(off) <= 0.25 * ms.beatPeriodSec) cuts.back() = hitBar;
  }
  cuts.push_back(nb);
  std::vector<double> secLow, secHigh;
  const auto meanOf = [&](const std::vector<double>& v, int a, int b) {
    double s = 0;
    for (int i = a; i < b; ++i) s += v[static_cast<size_t>(i)];
    return s / (b - a);
  };
  for (size_t i = 0; i + 1 < cuts.size(); ++i) {
    MusicSection s;
    s.startBar = cuts[i];
    s.endBar = cuts[i + 1];
    s.startSec = i == 0 ? 0.0 : bars[static_cast<size_t>(cuts[i])].start;
    s.endSec = i + 2 == cuts.size() ? ms.durationSec : bars[static_cast<size_t>(cuts[i + 1])].start;
    s.energyDb = meanOf(barF, s.startBar, s.endBar);
    secLow.push_back(meanOf(barL, s.startBar, s.endBar));
    // the band above 150 Hz, as mean-square: full band less the low band
    double hf = 0;
    for (int q = s.startBar; q < s.endBar; ++q) hf += std::max(0.0, std::pow(10.0, barF[static_cast<size_t>(q)] / 10.0) - std::pow(10.0, barL[static_cast<size_t>(q)] / 10.0));
    secHigh.push_back(dbOf(hf / (s.endBar - s.startBar)));
    ms.sections.push_back(s);
  }
  double lo = kInf, hi = -kInf, loLow = kInf, hiLow = -kInf;
  for (size_t i = 0; i < ms.sections.size(); ++i) {
    lo = std::min(lo, ms.sections[i].energyDb);
    hi = std::max(hi, ms.sections[i].energyDb);
    loLow = std::min(loLow, secLow[i]);
    hiLow = std::max(hiLow, secLow[i]);
  }
  // reference low band: the median over the loud sections (full band within 6 dB of the loudest), so one huge section cannot set it
  std::vector<double> loudLow;
  for (size_t i = 0; i < ms.sections.size(); ++i)
    if (ms.sections[i].energyDb >= hi - 6.0) loudLow.push_back(secLow[i]);
  std::sort(loudLow.begin(), loudLow.end());
  const double refLow = loudLow.empty() ? hiLow : (loudLow.size() % 2 ? loudLow[loudLow.size() / 2] : 0.5 * (loudLow[loudLow.size() / 2 - 1] + loudLow[loudLow.size() / 2]));
  const size_t ns = ms.sections.size();
  // A breakdown can keep its bass: a body section that sits 2.5 dB or more under both neighbouring body sections, in the full band or in the
  // band above 150 Hz, is a dip between two peaks (a build only climbs, so it is under its next section alone). An intro or outro neighbour
  // is quieter, so it never makes a dip.
  const auto dipsBetweenPeaks = [&](size_t i) {
    if (i == 0 || i + 1 >= ns) return false;
    const auto dip = [&](const std::vector<double>& v) { return std::min(v[i - 1], v[i + 1]) - v[i] >= 2.5; };
    std::vector<double> full;
    for (const MusicSection& x : ms.sections) full.push_back(x.energyDb);
    return dip(full) || dip(secHigh);
  };
  for (size_t i = 0; i < ns; ++i) {
    MusicSection& s = ms.sections[i];
    // relative level in each band; a band whose level never moves by 3 dB says nothing against the section, so the louder reading counts
    const double relFull = hi - lo >= 3.0 ? (s.energyDb - lo) / (hi - lo) : 1.0, relLow = hiLow - loLow >= 3.0 ? (secLow[i] - loLow) / (hiLow - loLow) : 1.0;
    s.relEnergy = std::max(relFull, relLow);
    const bool lowDrop = refLow - secLow[i] >= 6.0; // the kick / sub is gone, whatever the full-band level does
    if (hi - lo < 3.0 && hiLow - loLow < 3.0) s.label = "main";
    else if (i == 0 && (lowDrop || s.relEnergy < 0.66)) s.label = "intro";
    else if (i + 1 == ns && (lowDrop || s.relEnergy < 0.66)) s.label = "outro";
    else if (lowDrop || dipsBetweenPeaks(i)) s.label = "breakdown";
    else if (s.relEnergy >= 0.66 || refLow - secLow[i] < 3.0) s.label = "drop";
    else if (s.relEnergy < 0.4) s.label = "breakdown";
    else s.label = "build";
  }
  return ms;
}

} // namespace sf::audio
