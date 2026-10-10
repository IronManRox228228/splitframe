#pragma once

// Built-in insert effects: own float DSP, interleaved stereo, per-channel state, sample-rate aware.
//
// Everything here is strictly per sample (no block-size dependent behaviour) so a render comes out
// identical whatever the engine's block size or seek pattern. Parameters are plain numbers addressed
// by name; strings (filter types, detector mode) pick an entry from the parameter's choice list and
// booleans are 0/1. The mixer sets automated parameters on a 16-sample grid aligned to the timeline
// (see mixer.cpp).

#include "core/schema.h"

#include <QByteArray>
#include <QString>

#include <cmath>

#include <memory>
#include <vector>

namespace sf::audio {

struct ParamDef {
  const char* name;
  double def;
  double min;
  double max;
  const char* choices = nullptr; // comma separated names for enumerated parameters
};

class Effect {
public:
  virtual ~Effect() = default;
  // Sets the sample rate and clears all state.
  virtual void prepare(double sampleRate) = 0;
  virtual void reset() = 0;
  virtual const std::vector<ParamDef>& paramDefs() const = 0;
  // false for an unknown parameter
  virtual bool setParam(const QString& name, double value) = 0;
  virtual bool setParamString(const QString& name, const QString& value) = 0;
  virtual double param(const QString& name) const = 0;
  // Processes `frames` interleaved stereo frames in place. `sidechain` (same layout) keys the detector of
  // dynamics effects; null = key on the input.
  virtual void process(float* io, int frames, const float* sidechain) = 0;
  virtual bool usesSidechain() const { return false; }
  // Samples by which the output lags the input (lookahead); the mixer compensates the master chain.
  virtual int latencySamples() const { return 0; }
  // Plugin state (hosted plugins); empty for built-ins.
  virtual QByteArray saveState() { return {}; }
  virtual bool loadState(const QByteArray&) { return false; }
  // Gain reduction in dB right now (dynamics), for metering
  virtual double gainReductionDb() const { return 0; }
};

// "eq" "compressor" "limiter" "gate" "deesser" "reverb"; null for an unknown type ("clap" is created by
// the plugin host, see clap_host.h). Parameters from `params` are applied.
std::unique_ptr<Effect> makeEffect(const QString& type, double sampleRate, const EffectParams& params = {});
// Applies params of any variant kind; unknown names are ignored (returns how many were applied).
int applyParams(Effect& fx, const EffectParams& params);

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double lin) { return 20.0 * std::log10(lin < 1e-12 ? 1e-12 : lin); }

// Compressor static curve: gain reduction in dB (>= 0) for an input level, soft knee centred on the threshold.
double compressorReductionDb(double levelDb, double thresholdDb, double ratio, double kneeDb);

// ---- constant-power pan ----
// Centre is unity on both channels (so a default strip leaves the signal alone) and gl^2 + gr^2 == 2
// everywhere: hard left gl = sqrt(2), gr = 0.
struct PanGains {
  float l, r;
};
PanGains panGains(double pan);

} // namespace sf::audio
