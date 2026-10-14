#pragma once

// Measured audio: pure functions over interleaved 48 kHz stereo float buffers, no engine and no Qt, so they test on synthetic
// signals. Everything here is deterministic DSP (BS.1770 loudness from loudness.h, block energies, onset flux + autocorrelation);
// the engine commands (engine/cmd_audio.cpp) call it, then decide, then verify with the same functions.
//
//   MixAnalyzer     loudness, LRA, true peak, clipping runs, noise floor and a 1/s short-term curve, in any chunking
//   EnergyGate      speech ranges from block energy, when there is no footage index
//   EnergyMeter     mean-square level of a signal over chosen ranges (for the speech-over-music margin)
//   duckKeyframes   volume automation that dips a track under given ranges
//   analyseMusic    tempo, beats, downbeats (4/4) and coarse sections of a piece of music

#include "audio/loudness.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sf::audio {

constexpr int kAnalysisRate = 48000;
// A sample at or above this magnitude is counted as clipped (a hard-clipped or over-unity mix).
constexpr float kClipLevel = 0.9999f;

// ---------------------------------------------------------------- whole-mix measurement

struct ClipRun {
  std::int64_t startSample = 0; // first clipped frame of the run
  std::int64_t endSample = 0;   // last clipped frame of the run
  std::int64_t samples = 0;     // clipped sample values in the run (both channels)
};

struct MixMeasurement {
  LoudnessResult loudness; // integrated, range, true peak, sample peak
  double noiseFloorDb = -100.0; // 10th percentile of 50 ms RMS blocks, dBFS (-100 = digital silence)
  std::int64_t clippedSamples = 0;
  std::vector<ClipRun> clipRuns; // clipped frames closer than 50 ms form one run; at most kMaxClipRuns are kept
  std::vector<double> shortTermLufs; // one value per full second fed (value k is the 3 s window ending at second k+1), -inf before 3 s
  std::int64_t frames = 0;
  double seconds() const { return static_cast<double>(frames) / kAnalysisRate; }
};
constexpr size_t kMaxClipRuns = 64;

class MixAnalyzer {
public:
  // Interleaved stereo, any chunking: the result does not depend on how the signal is split.
  void process(const float* interleaved, std::int64_t frames);
  MixMeasurement result() const;

private:
  LoudnessMeter meter_;
  std::int64_t frames_ = 0;
  std::int64_t inSecond_ = 0;
  std::vector<double> shortTerm_;
  // noise floor blocks
  std::int64_t inBlock_ = 0;
  double blockSum_ = 0;
  std::vector<double> blockDb_;
  // clipping
  std::int64_t clipped_ = 0;
  std::int64_t lastClipFrame_ = -1000000;
  bool keepCur_ = false;
  std::vector<ClipRun> runs_;
};

MixMeasurement measureMix(const float* interleaved, std::int64_t frames);

// ---------------------------------------------------------------- ranges

struct Span {
  std::int64_t start = 0; // inclusive
  std::int64_t end = 0;   // exclusive
  bool operator==(const Span&) const = default;
};

// Sorted, overlapping spans joined, and spans whose gap is below `minGap` joined too. Empty spans are dropped.
std::vector<Span> mergeSpans(std::vector<Span> spans, std::int64_t minGap);

// Speech-like activity from block energy: 50 ms blocks above a threshold between the noise floor and the loud level
// (never below `floorDb`), runs shorter than `minSpeechMs` dropped. Ranges are in samples from the first fed frame.
class EnergyGate {
public:
  void process(const float* interleaved, std::int64_t frames);
  std::vector<Span> ranges(double minSpeechMs = 100.0, double floorDb = -55.0) const;
  double thresholdDb(double floorDb = -55.0) const;

private:
  std::int64_t inBlock_ = 0;
  double blockSum_ = 0;
  std::vector<double> blockDb_;
};
constexpr std::int64_t kGateBlock = 2400; // 50 ms

// Mean-square level (unweighted, both channels) of whatever is fed, in dB; -inf for silence.
class EnergyMeter {
public:
  void process(const float* interleaved, std::int64_t frames);
  double db() const;
  std::int64_t frames() const { return frames_; }

private:
  double sum_ = 0;
  std::int64_t frames_ = 0;
};

// speech - music in dB. +inf when the music is silent under a sounding speech, -inf when the speech is silent.
double marginDb(double speechDb, double musicDb);

// ---------------------------------------------------------------- ducking

struct DuckParams {
  double depthDb = 9.0;      // how far the music dips under speech
  double attackMs = 150.0;   // the dip is complete when speech starts: the ramp down starts this long before
  double releaseMs = 500.0;  // time to come back after speech ends
  double baseGain = 1.0;     // the music's fader value outside speech
};

struct DuckKey {
  std::int64_t frame = 0;
  double value = 1.0; // absolute linear gain (volume automation replaces the static fader)
};

// Keyframes (project frames) that hold `baseGain` outside the ranges and baseGain * 10^(-depth/20) inside, with linear ramps.
// Ranges closer than attack + release stay ducked across the gap. `ranges` are project frames and should be merged already.
std::vector<DuckKey> duckKeyframes(const std::vector<Span>& ranges, const DuckParams& p, double fps);

// ---------------------------------------------------------------- music structure

struct MusicSection {
  double startSec = 0, endSec = 0;
  int startBar = 0, endBar = 0; // bar indices, end exclusive
  std::string label;            // intro | build | drop | breakdown | outro | main
  double energyDb = -100;       // mean bar RMS, dBFS
  double relEnergy = 0;         // 0 = quietest section, 1 = loudest
};

struct MusicStructure {
  bool valid = false;
  std::string note;
  double durationSec = 0;
  double bpm = 0;
  double tempoConfidence = 0; // normalised autocorrelation of the onset envelope at the beat period, 0..1
  double beatPeriodSec = 0;
  int downbeatPhase = 0;      // beat index (0..3) that starts a bar
  std::string downbeatEvidence; // how the bar phase was chosen (low-band kick share and section changes per phase)
  std::vector<double> beats;     // seconds
  std::vector<double> downbeats; // seconds, every 4th beat from downbeatPhase
  std::vector<MusicSection> sections;
};

// Analyses at most the first 10 minutes. Time 0 is the first fed frame.
MusicStructure analyseMusic(const float* interleaved, std::int64_t frames);

// Sections use two bands (full and < 150 Hz); a breakdown is a section whose low band is 6 dB under the loudest section's.
// Exposed for tests: spectral-flux onset envelope (detrended, non-negative) at kOnsetRate values per second.
constexpr int kOnsetHop = 240; // 5 ms
std::vector<float> onsetEnvelope(const float* interleaved, std::int64_t frames);

} // namespace sf::audio
