#include "audio/effects.h"

#include "audio/biquad.h"

#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>

namespace sf::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

// One-pole smoothing coefficient for a time constant in ms (0 = instant)
double timeCoef(double ms, double sr) { return ms <= 0 ? 0.0 : std::exp(-1.0 / (ms * 0.001 * sr)); }

double flush(double v) { return std::fabs(v) < 1e-20 ? 0.0 : v; }

// Shared parameter storage: effects read p(i) after calling takeDirty().
class ParamEffect : public Effect {
public:
  const std::vector<ParamDef>& paramDefs() const override { return defs_; }
  bool setParam(const QString& name, double value) override {
    const int i = indexOf(name);
    if (i < 0) return false;
    const ParamDef& d = defs_[static_cast<size_t>(i)];
    value = std::min(d.max, std::max(d.min, value));
    if (value != vals_[static_cast<size_t>(i)]) {
      vals_[static_cast<size_t>(i)] = value;
      dirty_ = true;
    }
    return true;
  }
  bool setParamString(const QString& name, const QString& value) override {
    const int i = indexOf(name);
    if (i < 0) return false;
    const char* choices = defs_[static_cast<size_t>(i)].choices;
    if (!choices) return false;
    const QStringList list = QString::fromLatin1(choices).split(QLatin1Char(','));
    const int idx = static_cast<int>(list.indexOf(value));
    if (idx < 0) return false;
    return setParam(name, idx);
  }
  double param(const QString& name) const override {
    const int i = indexOf(name);
    return i < 0 ? 0.0 : vals_[static_cast<size_t>(i)];
  }

protected:
  explicit ParamEffect(std::vector<ParamDef> defs) : defs_(std::move(defs)) {
    for (const ParamDef& d : defs_) vals_.push_back(d.def);
  }
  double p(int i) const { return vals_[static_cast<size_t>(i)]; }
  bool takeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
  }
  void markDirty() { dirty_ = true; }

private:
  int indexOf(const QString& name) const {
    for (size_t i = 0; i < defs_.size(); ++i)
      if (name == QLatin1StringView(defs_[i].name)) return static_cast<int>(i);
    return -1;
  }
  std::vector<ParamDef> defs_;
  std::vector<double> vals_;
  bool dirty_ = true;
};

// ---------------------------------------------------------------- EQ

constexpr int kEqBands = 6;
constexpr const char* kEqTypes = "lowpass,highpass,bandpass,notch,peak,lowshelf,highshelf";

std::vector<ParamDef> eqDefs() {
  // ParamDef keeps raw name pointers, so the strings must outlive every Eq
  static const std::vector<std::array<std::string, 5>> names = [] {
    std::vector<std::array<std::string, 5>> v;
    for (int i = 0; i < kEqBands; ++i) {
      const std::string b = "b" + std::to_string(i) + ".";
      v.push_back({b + "on", b + "type", b + "freq", b + "gain", b + "q"});
    }
    return v;
  }();
  struct Def {
    double type, freq, gain, q;
  };
  static const Def bands[kEqBands] = {{1, 80, 0, 0.7071}, {5, 120, 0, 0.7071}, {4, 400, 0, 1.0},
                                      {4, 1500, 0, 1.0},  {4, 5000, 0, 1.0},   {6, 10000, 0, 0.7071}};
  std::vector<ParamDef> d;
  d.push_back({"gain", 0, -48, 48});
  for (int i = 0; i < kEqBands; ++i) {
    const auto& n = names[static_cast<size_t>(i)];
    d.push_back({n[0].c_str(), 0, 0, 1});
    d.push_back({n[1].c_str(), bands[i].type, 0, 6, kEqTypes});
    d.push_back({n[2].c_str(), bands[i].freq, 10, 24000});
    d.push_back({n[3].c_str(), bands[i].gain, -30, 30});
    d.push_back({n[4].c_str(), bands[i].q, 0.1, 18});
  }
  return d;
}

class Eq final : public ParamEffect {
public:
  Eq() : ParamEffect(eqDefs()) {}
  void prepare(double sr) override {
    sr_ = sr;
    reset();
    markDirty();
  }
  void reset() override {
    for (auto& ch : f_)
      for (Biquad& b : ch) b.reset();
  }
  void process(float* io, int frames, const float*) override {
    if (takeDirty()) update();
    if (active_ == 0 && gain_ == 1.0f) return;
    for (int i = 0; i < frames; ++i) {
      for (int c = 0; c < 2; ++c) {
        float x = io[i * 2 + c];
        for (int b = 0; b < kEqBands; ++b)
          if (on_[b]) x = f_[c][b].process(x);
        io[i * 2 + c] = x * gain_;
      }
    }
  }

private:
  void update() {
    gain_ = static_cast<float>(dbToLin(p(0)));
    active_ = 0;
    for (int b = 0; b < kEqBands; ++b) {
      const int o = 1 + b * 5;
      on_[b] = p(o) >= 0.5;
      if (!on_[b]) continue;
      ++active_;
      const auto c = designBiquad(static_cast<BiquadType>(static_cast<int>(p(o + 1))), sr_, p(o + 2), p(o + 3), p(o + 4));
      f_[0][b].set(c);
      f_[1][b].set(c);
    }
  }
  double sr_ = 48000;
  float gain_ = 1;
  int active_ = 0;
  bool on_[kEqBands] = {};
  Biquad f_[2][kEqBands];
};

// ---------------------------------------------------------------- compressor

class Compressor final : public ParamEffect {
  enum { Threshold, Ratio, Attack, Release, Knee, Makeup, Mode, Mix };

public:
  Compressor()
      : ParamEffect({{"threshold", -18, -80, 0},
                     {"ratio", 4, 1, 100},
                     {"attack", 10, 0, 500},
                     {"release", 120, 1, 5000},
                     {"knee", 6, 0, 36},
                     {"makeup", 0, -24, 36},
                     {"mode", 1, 0, 1, "peak,rms"},
                     {"mix", 1, 0, 1}}) {}
  void prepare(double sr) override {
    sr_ = sr;
    reset();
    markDirty();
  }
  void reset() override {
    ms_ = 0;
    gr_ = 0;
  }
  bool usesSidechain() const override { return true; }
  double gainReductionDb() const override { return gr_; }
  void process(float* io, int frames, const float* sc) override {
    if (takeDirty()) {
      att_ = timeCoef(p(Attack), sr_);
      rel_ = timeCoef(p(Release), sr_);
      rmsCoef_ = timeCoef(10.0, sr_);
    }
    const double thr = p(Threshold), ratio = p(Ratio), knee = p(Knee), makeup = p(Makeup), mix = p(Mix);
    const bool rms = p(Mode) >= 0.5;
    const float* key = sc ? sc : io;
    for (int i = 0; i < frames; ++i) {
      const double kl = key[i * 2], kr = key[i * 2 + 1];
      double lvl;
      if (rms) {
        ms_ = flush(rmsCoef_ * ms_ + (1 - rmsCoef_) * 0.5 * (kl * kl + kr * kr));
        lvl = std::sqrt(ms_);
      } else {
        lvl = std::max(std::fabs(kl), std::fabs(kr));
      }
      const double target = compressorReductionDb(linToDb(lvl), thr, ratio, knee);
      const double c = target > gr_ ? att_ : rel_;
      gr_ = flush(c * gr_ + (1 - c) * target);
      const float g = static_cast<float>(dbToLin(makeup - gr_));
      for (int ch = 0; ch < 2; ++ch) {
        const float dry = io[i * 2 + ch];
        const float wet = dry * g;
        io[i * 2 + ch] = mix >= 1.0 ? wet : static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  double sr_ = 48000, att_ = 0, rel_ = 0, rmsCoef_ = 0, ms_ = 0, gr_ = 0;
};

// ---------------------------------------------------------------- gate / expander

// Downward expander with hold and hysteresis; ratio 20+ behaves as a gate, `range` caps the attenuation.
class Gate final : public ParamEffect {
  enum { Threshold, Ratio, Attack, Hold, Release, Range };

public:
  Gate()
      : ParamEffect({{"threshold", -50, -90, 0},
                     {"ratio", 20, 1, 100},
                     {"attack", 1, 0, 200},
                     {"hold", 50, 0, 2000},
                     {"release", 150, 1, 5000},
                     {"range", -80, -120, 0}}) {}
  void prepare(double sr) override {
    sr_ = sr;
    reset();
    markDirty();
  }
  void reset() override {
    env_ = 0;
    gain_ = 0;
    hold_ = 0;
    open_ = false;
  }
  bool usesSidechain() const override { return true; }
  double gainReductionDb() const override { return -linToDb(gain_); }
  void process(float* io, int frames, const float* sc) override {
    if (takeDirty()) {
      att_ = timeCoef(p(Attack), sr_);
      rel_ = timeCoef(p(Release), sr_);
      holdSamples_ = static_cast<int>(p(Hold) * 0.001 * sr_);
      envCoef_ = timeCoef(0.5, sr_); // level follower: instant attack, ~0.5 ms release
    }
    const double thr = p(Threshold), ratio = p(Ratio), range = p(Range);
    const float* key = sc ? sc : io;
    for (int i = 0; i < frames; ++i) {
      const double lvl = std::max(std::fabs(static_cast<double>(key[i * 2])), std::fabs(static_cast<double>(key[i * 2 + 1])));
      env_ = flush(lvl > env_ ? lvl : envCoef_ * env_ + (1 - envCoef_) * lvl);
      const double lvlDb = linToDb(env_);
      // 3 dB hysteresis: opens at the threshold, closes 3 dB below it after the hold time
      if (lvlDb >= thr) {
        open_ = true;
        hold_ = holdSamples_;
      } else if (open_ && lvlDb < thr - 3.0) {
        if (hold_ > 0) --hold_;
        else open_ = false;
      }
      double targetDb = 0;
      if (!open_) targetDb = std::max(range, (lvlDb - thr) * (ratio - 1.0));
      const double target = dbToLin(targetDb);
      const double c = target > gain_ ? att_ : rel_;
      gain_ = flush(c * gain_ + (1 - c) * target);
      io[i * 2] = static_cast<float>(io[i * 2] * gain_);
      io[i * 2 + 1] = static_cast<float>(io[i * 2 + 1] * gain_);
    }
  }

private:
  double sr_ = 48000, att_ = 0, rel_ = 0, envCoef_ = 0, env_ = 0, gain_ = 0;
  int holdSamples_ = 0, hold_ = 0;
  bool open_ = false;
};

// ---------------------------------------------------------------- de-esser

class DeEsser final : public ParamEffect {
  enum { Freq, Threshold, Ratio, Attack, Release };

public:
  DeEsser()
      : ParamEffect({{"freq", 6000, 2000, 16000}, {"threshold", -30, -80, 0}, {"ratio", 4, 1, 20}, {"attack", 1, 0, 50}, {"release", 60, 1, 1000}}) {}
  void prepare(double sr) override {
    sr_ = sr;
    reset();
    markDirty();
  }
  void reset() override {
    for (auto& ch : hp_)
      for (Biquad& b : ch) b.reset();
    for (auto& ch : lp_)
      for (Biquad& b : ch) b.reset();
    for (Biquad& b : kHp_) b.reset();
    env_ = 0;
    gr_ = 0;
  }
  bool usesSidechain() const override { return true; }
  double gainReductionDb() const override { return gr_; }
  void process(float* io, int frames, const float* sc) override {
    if (takeDirty()) {
      const auto c = designBiquad(BiquadType::HighPass, sr_, p(Freq), 0, 0.7071);
      const auto lc = designBiquad(BiquadType::LowPass, sr_, p(Freq), 0, 0.7071);
      for (auto& ch : hp_)
        for (Biquad& b : ch) b.set(c);
      for (auto& ch : lp_)
        for (Biquad& b : ch) b.set(lc);
      for (Biquad& b : kHp_) b.set(c);
      att_ = timeCoef(p(Attack), sr_);
      rel_ = timeCoef(p(Release), sr_);
      envCoef_ = timeCoef(1.0, sr_);
    }
    const double thr = p(Threshold), slope = 1.0 - 1.0 / p(Ratio);
    const float* key = sc ? sc : io;
    for (int i = 0; i < frames; ++i) {
      const double kl = kHp_[0].process(key[i * 2]), kr = kHp_[1].process(key[i * 2 + 1]);
      const double lvl = std::max(std::fabs(kl), std::fabs(kr));
      env_ = flush(lvl > env_ ? lvl : envCoef_ * env_ + (1 - envCoef_) * lvl);
      const double target = std::max(0.0, (linToDb(env_) - thr) * slope);
      const double c = target > gr_ ? att_ : rel_;
      gr_ = flush(c * gr_ + (1 - c) * target);
      const float g = static_cast<float>(dbToLin(-gr_));
      for (int ch = 0; ch < 2; ++ch) {
        const float x = io[i * 2 + ch];
        // Linkwitz-Riley split (two cascaded Butterworth biquads per side): low + high sums flat, so turning
        // the high band down really attenuates the sibilance
        const float high = hp_[ch][1].process(hp_[ch][0].process(x));
        const float low = lp_[ch][1].process(lp_[ch][0].process(x));
        io[i * 2 + ch] = low + high * g;
      }
    }
  }

private:
  double sr_ = 48000, att_ = 0, rel_ = 0, envCoef_ = 0, env_ = 0, gr_ = 0;
  Biquad hp_[2][2], lp_[2][2], kHp_[2];
};

// ---------------------------------------------------------------- limiter

// Lookahead brick-wall. The gain needed to keep each sample (and the cubic-interpolated point between
// neighbours, a cheap stand-in for a true-peak detector) under the ceiling is min-filtered over the
// lookahead window w and then averaged over the same window, so the gain eases down before the peak
// arrives and never exceeds what any sample in the window needs. Output lags the input by w - 1 samples.
class Limiter final : public ParamEffect {
  enum { Ceiling, Release, Lookahead, Input };

public:
  Limiter() : ParamEffect({{"ceiling", -1, -30, 0}, {"release", 80, 1, 2000}, {"lookahead", 5, 0.1, 20}, {"input", 0, -24, 24}}) {}
  void prepare(double sr) override {
    sr_ = sr;
    w_ = window();
    reset();
    markDirty();
  }
  void reset() override {
    delay_.assign(static_cast<size_t>(w_ - 1) * 2, 0.0f);
    avgBuf_.assign(static_cast<size_t>(w_), 1.0);
    avgSum_ = static_cast<double>(w_);
    minq_.clear();
    pos_ = 0;
    r_ = 1;
    gr_ = 0;
    prevL_.fill(0);
    prevR_.fill(0);
  }
  int latencySamples() const override { return w_ - 1; }
  double gainReductionDb() const override { return gr_; }
  void process(float* io, int frames, const float*) override {
    if (takeDirty()) {
      const int w = window();
      if (w != w_) {
        w_ = w;
        reset();
      }
      rel_ = timeCoef(p(Release), sr_);
      ceil_ = dbToLin(p(Ceiling));
      in_ = static_cast<float>(dbToLin(p(Input)));
    }
    const size_t ring = static_cast<size_t>(w_ - 1);
    for (int i = 0; i < frames; ++i) {
      const float xl = io[i * 2] * in_, xr = io[i * 2 + 1] * in_;
      float dl = xl, dr = xr;
      if (ring > 0) {
        const size_t d = static_cast<size_t>(pos_) % ring;
        dl = delay_[d * 2];
        dr = delay_[d * 2 + 1];
        delay_[d * 2] = xl;
        delay_[d * 2 + 1] = xr;
      }
      double peak = std::max(std::fabs(static_cast<double>(xl)), std::fabs(static_cast<double>(xr)));
      peak = std::max(peak, interSamplePeak(xl, xr));
      const double req = peak > ceil_ ? ceil_ / peak : 1.0;

      // sliding minimum over the last w requests
      while (!minq_.empty() && minq_.back().second >= req) minq_.pop_back();
      minq_.emplace_back(pos_, req);
      while (minq_.front().first <= pos_ - w_) minq_.pop_front();
      const double m = minq_.front().second;

      // release: recover toward 1 slowly, never above the window minimum
      r_ = std::min(m, rel_ * r_ + (1 - rel_));

      // boxcar average over the window
      const size_t a = static_cast<size_t>(pos_) % static_cast<size_t>(w_);
      avgSum_ += r_ - avgBuf_[a];
      avgBuf_[a] = r_;
      const double g = std::min(1.0, avgSum_ / w_);
      gr_ = -linToDb(g);
      ++pos_;
      io[i * 2] = static_cast<float>(dl * g);
      io[i * 2 + 1] = static_cast<float>(dr * g);
    }
  }

private:
  int window() const { return std::max(1, static_cast<int>(std::lround(p(Lookahead) * 0.001 * sr_))); }
  // Catmull-Rom midpoint between the two newest-but-one samples, per channel
  double interSamplePeak(float xl, float xr) {
    auto push = [](std::array<float, 4>& h, float x) {
      h[0] = h[1];
      h[1] = h[2];
      h[2] = h[3];
      h[3] = x;
      return std::fabs((-static_cast<double>(h[0]) + 9.0 * h[1] + 9.0 * h[2] - static_cast<double>(h[3])) / 16.0);
    };
    return std::max(push(prevL_, xl), push(prevR_, xr));
  }

  double sr_ = 48000, rel_ = 0, ceil_ = 0.89, r_ = 1, avgSum_ = 0, gr_ = 0;
  float in_ = 1;
  int w_ = 240;
  std::int64_t pos_ = 0;
  std::vector<float> delay_;
  std::vector<double> avgBuf_;
  std::deque<std::pair<std::int64_t, double>> minq_;
  std::array<float, 4> prevL_{}, prevR_{};
};

// ---------------------------------------------------------------- reverb (Freeverb)

// Jezar's public-domain Freeverb topology: 8 parallel damped feedback combs into 4 series allpasses
// per channel, the right channel offset by 23 samples. Delay lengths are scaled from 44.1 kHz.
class Reverb final : public ParamEffect {
  enum { Room, Damp, Wet, Dry, Width, PreDelay };

  struct Comb {
    std::vector<float> buf;
    size_t idx = 0;
    float store = 0;
    float process(float in, float feedback, float damp) {
      const float out = buf[idx];
      store = static_cast<float>(flush(out * (1 - damp) + store * damp));
      buf[idx] = static_cast<float>(flush(in + store * feedback));
      if (++idx >= buf.size()) idx = 0;
      return out;
    }
  };
  struct Allpass {
    std::vector<float> buf;
    size_t idx = 0;
    float process(float in) {
      const float b = buf[idx];
      const float out = b - in;
      buf[idx] = static_cast<float>(flush(in + b * 0.5f));
      if (++idx >= buf.size()) idx = 0;
      return out;
    }
  };

public:
  Reverb() : ParamEffect({{"room", 0.5, 0, 1}, {"damp", 0.5, 0, 1}, {"wet", 0.3, 0, 1}, {"dry", 1, 0, 1}, {"width", 1, 0, 1}, {"predelay", 0, 0, 200}}) {}
  void prepare(double sr) override {
    sr_ = sr;
    static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allpass[4] = {556, 441, 341, 225};
    const double k = sr / 44100.0;
    for (int ch = 0; ch < 2; ++ch) {
      const int spread = ch == 0 ? 0 : 23;
      for (int i = 0; i < 8; ++i) combs_[ch][i].buf.assign(static_cast<size_t>(std::lround((combs[i] + spread) * k)), 0.0f);
      for (int i = 0; i < 4; ++i) allpass_[ch][i].buf.assign(static_cast<size_t>(std::lround((allpass[i] + spread) * k)), 0.0f);
    }
    pre_.assign(static_cast<size_t>(std::lround(0.2 * sr)) + 1, 0.0f);
    reset();
    markDirty();
  }
  void reset() override {
    for (auto& ch : combs_)
      for (Comb& c : ch) {
        std::fill(c.buf.begin(), c.buf.end(), 0.0f);
        c.idx = 0;
        c.store = 0;
      }
    for (auto& ch : allpass_)
      for (Allpass& a : ch) {
        std::fill(a.buf.begin(), a.buf.end(), 0.0f);
        a.idx = 0;
      }
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    preIdx_ = 0;
  }
  void process(float* io, int frames, const float*) override {
    if (takeDirty()) {
      feedback_ = static_cast<float>(p(Room) * 0.28 + 0.7);
      damp_ = static_cast<float>(p(Damp) * 0.4);
      const double wet = p(Wet) * 3.0;
      wet1_ = static_cast<float>(wet * (p(Width) / 2 + 0.5));
      wet2_ = static_cast<float>(wet * ((1 - p(Width)) / 2));
      dry_ = static_cast<float>(p(Dry));
      preSamples_ = std::min(pre_.size() - 1, static_cast<size_t>(std::lround(p(PreDelay) * 0.001 * sr_)));
    }
    for (int i = 0; i < frames; ++i) {
      const float l = io[i * 2], r = io[i * 2 + 1];
      float in = (l + r) * 0.015f;
      if (preSamples_ > 0) {
        pre_[preIdx_] = in;
        const size_t rd = (preIdx_ + pre_.size() - preSamples_) % pre_.size();
        in = pre_[rd];
        preIdx_ = (preIdx_ + 1) % pre_.size();
      }
      float outL = 0, outR = 0;
      for (int c = 0; c < 8; ++c) {
        outL += combs_[0][c].process(in, feedback_, damp_);
        outR += combs_[1][c].process(in, feedback_, damp_);
      }
      for (int a = 0; a < 4; ++a) {
        outL = allpass_[0][a].process(outL);
        outR = allpass_[1][a].process(outR);
      }
      io[i * 2] = l * dry_ + outL * wet1_ + outR * wet2_;
      io[i * 2 + 1] = r * dry_ + outR * wet1_ + outL * wet2_;
    }
  }

private:
  double sr_ = 48000;
  float feedback_ = 0.84f, damp_ = 0.2f, wet1_ = 0, wet2_ = 0, dry_ = 1;
  Comb combs_[2][8];
  Allpass allpass_[2][4];
  std::vector<float> pre_;
  size_t preIdx_ = 0, preSamples_ = 0;
};

} // namespace

double compressorReductionDb(double levelDb, double thr, double ratio, double knee) {
  const double over = levelDb - thr;
  double out;
  if (2 * over < -knee) {
    out = levelDb;
  } else if (knee > 0 && 2 * std::fabs(over) <= knee) {
    const double t = over + knee / 2;
    out = levelDb + (1.0 / ratio - 1.0) * t * t / (2 * knee);
  } else {
    out = thr + over / ratio;
  }
  return std::max(0.0, levelDb - out);
}

PanGains panGains(double pan) {
  pan = std::min(1.0, std::max(-1.0, pan));
  const double theta = (pan + 1.0) * kPi / 4.0;
  const double s = std::sqrt(2.0);
  return {static_cast<float>(std::cos(theta) * s), static_cast<float>(std::sin(theta) * s)};
}

std::unique_ptr<Effect> makeEffect(const QString& type, double sr, const EffectParams& params) {
  std::unique_ptr<Effect> fx;
  if (type == QLatin1String("eq")) fx = std::make_unique<Eq>();
  else if (type == QLatin1String("compressor")) fx = std::make_unique<Compressor>();
  else if (type == QLatin1String("limiter")) fx = std::make_unique<Limiter>();
  else if (type == QLatin1String("gate")) fx = std::make_unique<Gate>();
  else if (type == QLatin1String("deesser")) fx = std::make_unique<DeEsser>();
  else if (type == QLatin1String("reverb")) fx = std::make_unique<Reverb>();
  if (!fx) return nullptr;
  fx->prepare(sr);
  applyParams(*fx, params);
  return fx;
}

int applyParams(Effect& fx, const EffectParams& params) {
  int n = 0;
  for (const auto& [name, v] : params) {
    bool ok = false;
    if (const double* d = std::get_if<double>(&v)) ok = fx.setParam(name, *d);
    else if (const bool* b = std::get_if<bool>(&v)) ok = fx.setParam(name, *b ? 1.0 : 0.0);
    else ok = fx.setParamString(name, std::get<QString>(v));
    if (ok) ++n;
  }
  return n;
}

} // namespace sf::audio
