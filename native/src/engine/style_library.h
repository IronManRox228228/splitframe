#pragma once

// Taste as data: looks (primary colour grades with guard rules), pacing profiles and mix profiles, compiled in as JSON resources
// (engine/styles/*.json, qrc prefix /sf/style) and checked against schemas built from the same jschema vocabulary the commands use.
// A look is expressed ONLY in the parameters primary colour correction supports (PrimaryGrade: exposure, temperature / tint, contrast,
// saturation, lift / gamma / gain); its guards are limits that colour.analyze / colour.check can measure. Pure functions: the engine
// commands (engine/cmd_style.cpp) measure, propose with these, and verify; a model only picks names and small bounded offsets.

#include "core/schema.h"
#include "engine/timeline_analysis.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <vector>

namespace sf::engine::style {

// ---------------------------------------------------------------- the library

// Limits a graded clip must stay inside, measured on the clip (colour.analyze metrics) or read off the grade itself.
struct LookGuards {
  double maxChromaP95 = 0.85;           // display chroma (max - min of R'G'B'), 95th percentile
  double maxGamutLimitPct = 5.0;        // percent of pixels at HSV saturation >= 0.95
  double maxLiftMaster = 0.08;          // shadow lift of the grade (master plus the largest channel lift)
  double blacksCrushedIncrease = 3.0;   // colour.check thresholds, percentage points
  double highlightsClippedIncrease = 2.0;
  double skinHueShiftDeg = 8.0;         // mean skin hue may move this far on the vectorscope
  double skinMinPct = 2.0;              // the skin guards apply when skin covers more than this
  double exposureShiftStops = 0.6;      // median exposure may move this far beyond the look's own exposure change
  double skinBandMin = 100.0, skinBandMax = 150.0; // the skin hue must stay in this band (vectorscope degrees, skin line 123)
};

struct Look {
  QString name, description;
  QStringList tags;
  QJsonObject params; // as written in the resource
  PrimaryGrade grade; // params as a grade
  LookGuards guards;
  QJsonObject guardsJson; // the guards as written (only the keys the look sets)
};

struct PacingProfile {
  QString name, description;
  double aslMin = 0, aslMax = 0;   // average shot length, seconds
  double cutsOnBeatMinPct = 0;     // share of cuts within +-2 frames of a beat; 0 = not music-driven, not checked
  double hookMaxSec = 0;           // the first cut comes within this
  double minShotSec = 0, maxShotSec = 0;
};

struct MixProfile {
  QString name, description;
  double targetLufs = -14, toleranceLu = 1, truePeakCeilingDb = -1;
  double duckMarginDb = 9;         // speech over music while speech plays
  double attackMs = 150, releaseMs = 500;
  std::optional<double> altTargetLufs;
  double minSpeechMarginDb() const { return std::max(1.0, duckMarginDb - 3.0); } // what audio.check accepts after ducking to duckMarginDb
};

// `base` with every key present in `g` (a look's guards object, or a caller's overrides) replaced.
LookGuards guardsFromJson(const QJsonObject& g, LookGuards base = {});

const std::vector<Look>& looks();
const std::vector<PacingProfile>& pacings();
const std::vector<MixProfile>& mixes();
const Look* findLook(const QString& name);
const PacingProfile* findPacing(const QString& name);
const MixProfile* findMix(const QString& name);
QStringList lookNames();
QStringList pacingNames();
QStringList mixNames();

QJsonObject toJson(const Look& l);
QJsonObject toJson(const PacingProfile& p);
QJsonObject toJson(const MixProfile& m);
// {kind, items[...], names[...]}
QJsonObject listJson(const QString& kind);

// ---------------------------------------------------------------- schemas and validation

// JSON schemas (the jschema subset) of one library item of each kind, of a whole resource file, and of the style spec a model fills.
QJsonObject lookSchema();
QJsonObject pacingSchema();
QJsonObject mixSchema();
QJsonObject resourceSchema(const QString& kind);
QJsonObject specSchema();
// Every problem in the compiled-in resources: parse errors, schema violations, duplicate names, ranges that do not hold together
// (min > max), grades outside the bounds item.color.set accepts. Empty = the library is sound.
QStringList validateLibrary();

// ---------------------------------------------------------------- looks

struct SpecOffsets {
  double warmth = 0;     // added to the grade's temperature
  double contrast = 0;   // contrast *= 1 + contrast
  double saturation = 0; // saturation *= 1 + saturation
};
inline constexpr double kMaxWarmthOffset = 0.2, kMaxContrastOffset = 0.2, kMaxSaturationOffset = 0.2;

// Partial grade JSON ({exposure, temperature, ..., lift: {master, r, g, b}, ...}) to a grade; missing keys are neutral.
PrimaryGrade gradeFromJson(const QJsonObject& params);
// Every field moved from neutral by `intensity` (0 = neutral, 1 = as given).
PrimaryGrade scaleGrade(const PrimaryGrade& g, double intensity);
// `delta` on top of `base`: stops, temperature, tint, lifts and offsets add; contrast, saturation, gammas and gains multiply. Clamped to the schema bounds.
PrimaryGrade layerGrade(const PrimaryGrade& base, const PrimaryGrade& delta);
PrimaryGrade applyOffsets(PrimaryGrade g, const SpecOffsets& o);
// The look at `intensity` with the offsets, layered on `existing` (nullopt: replaces whatever the clip had).
PrimaryGrade proposeLook(const Look& look, double intensity, const SpecOffsets& offsets, const std::optional<PrimaryGrade>& existing);
// Shadow lift of a grade as the guard reads it.
double shadowLift(const PrimaryGrade& g);

// ---------------------------------------------------------------- pacing

struct PacingViolation {
  QString rule; // asl_too_fast | asl_too_slow | beat_share | hook | min_shot | max_shot
  double measured = 0, limit = 0;
  std::optional<Frame> frame;
  QString message;
};

// `beatSharePct`: share of cuts within +-2 frames of a beat; nullopt when there is no music grid (the rule is then not checked).
std::vector<PacingViolation> checkPacing(const PacingProfile& p, const tl::ShotStats& stats, std::optional<double> beatSharePct, double fps);

} // namespace sf::engine::style
