#pragma once

// Taste as data: looks (primary colour grades with guard rules), pacing profiles and mix profiles, compiled in as JSON resources
// (engine/styles/*.json, qrc prefix /sf/style) and checked against schemas built from the same jschema vocabulary the commands use.
// A look is expressed ONLY in the parameters primary colour correction supports (PrimaryGrade: exposure, temperature / tint, contrast,
// saturation, lift / gamma / gain); its guards are limits that colour.analyze / colour.check can measure. Pure functions: the engine
// commands (engine/cmd_style.cpp) measure, propose with these, and verify; a model only picks names and small bounded offsets.

#include "core/schema.h"
#include "engine/timeline_analysis.h"
#include "render/color.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
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
  QStringList whenToUse, avoidWhen; // tags for style.suggest
  QJsonObject params; // as written in the resource
  PrimaryGrade grade; // params as a grade
  LookGuards guards;
  QJsonObject guardsJson; // the guards as written (only the keys the look sets)
};

struct PacingProfile {
  QString name, description;
  QStringList whenToUse, avoidWhen;
  double aslMin = 0, aslMax = 0;   // average shot length, seconds
  double cutsOnBeatMinPct = 0;     // share of cuts within +-2 frames of a beat; 0 = not music-driven, not checked
  double hookMaxSec = 0;           // the first cut comes within this
  double minShotSec = 0, maxShotSec = 0;
};

struct MixProfile {
  QString name, description;
  QStringList whenToUse, avoidWhen;
  double targetLufs = -14, toleranceLu = 1, truePeakCeilingDb = -1;
  double duckMarginDb = 9;         // speech over music while speech plays
  double attackMs = 150, releaseMs = 500;
  std::optional<double> altTargetLufs;
  double minSpeechMarginDb() const { return std::max(1.0, duckMarginDb - 3.0); } // what audio.check accepts after ducking to duckMarginDb
};

// One effect of an audio preset: a mixer insert type with the params it takes (numbers; strings pick from a choice list).
struct AudioEffectSpec {
  QString type; // eq compressor limiter gate deesser reverb
  QJsonObject params;
};
// An EQ / dynamics chain for a track strip (engine/styles/audio_presets.json).
struct AudioPreset {
  QString id, name, category, description;
  QStringList whenToUse, avoidWhen;
  std::vector<AudioEffectSpec> chain;
};

// One command step in a recipe: command name and its arguments (may contain placeholders).
struct RecipeStep {
  QString command;
  QJsonObject args;
};

// A named recipe (engine/styles/recipes.json): an atomic sequence of engine commands executed as one undo step.
struct Recipe {
  QString name, description;
  QStringList whenToUse, avoidWhen;
  std::vector<RecipeStep> steps;
  std::vector<RecipeStep> verify;
};

// `base` with every key present in `g` (a look's guards object, or a caller's overrides) replaced.
LookGuards guardsFromJson(const QJsonObject& g, LookGuards base = {});

const std::vector<Look>& looks();
const std::vector<PacingProfile>& pacings();
const std::vector<MixProfile>& mixes();
const std::vector<Recipe>& recipes();
const Look* findLook(const QString& name);
const PacingProfile* findPacing(const QString& name);
const MixProfile* findMix(const QString& name);
const Recipe* findRecipe(const QString& name);
const std::vector<AudioPreset>& audioPresets();
const AudioPreset* findAudioPreset(const QString& id);
QStringList audioPresetIds();
QJsonObject toJson(const AudioPreset& p);
QJsonObject toJson(const Recipe& r);
// Problems with one preset's chain against the real effect parameter ranges (unknown type / param, out of range, bad choice).
QStringList validateAudioPreset(const AudioPreset& p);
QStringList lookNames();
QStringList pacingNames();
QStringList mixNames();
QStringList recipeNames();

QJsonObject toJson(const Look& l);
QJsonObject toJson(const PacingProfile& p);
QJsonObject toJson(const MixProfile& m);
// {kind, items[...], names[...]}
QJsonObject listJson(const QString& kind);

// ---------------------------------------------------------------- suggestions

struct Suggestion {
  QString id;
  int score = 0; // overlap with the item's tags minus hits on its avoidWhen
  QStringList matched, avoided;
};
// Deterministic ranking: score = |query tags found in `positive`| - |query tags found in `avoid`|; a tag is found when it equals an entry or
// is a whole word of a multi-word entry (case-insensitive). Highest score first, ties by id. Every candidate is returned.
struct Candidate {
  QString id;
  QStringList positive, avoid;
};
std::vector<Suggestion> rankByTags(const std::vector<Candidate>& candidates, const QStringList& queryTags);

// ---------------------------------------------------------------- schemas and validation

// JSON schemas (the jschema subset) of one library item of each kind, of a whole resource file, and of the style spec a model fills.
QJsonObject lookSchema();
QJsonObject pacingSchema();
QJsonObject mixSchema();
QJsonObject recipeSchema();
QJsonObject resourceSchema(const QString& kind);
QJsonObject audioPresetSchema();
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

// ---------------------------------------------------------------- LUT export

// Output of the grade for one colour in the encoding of `inputSpace`: decode with that transfer, applyGrade (render/color.h, the CPU reference
// of the shader) in linear light, encode again, clamp to 0..1.
std::array<double, 3> gradeEncoded(const PrimaryGrade& g, render::Transfer transfer, const std::array<double, 3>& rgb);
// Adobe / Resolve .cube text: TITLE, LUT_3D_SIZE, DOMAIN_MIN / MAX, then size^3 lines, red fastest, 6 decimals. `inputSpace`: srgb (default) | rec709 |
// bt1886 | gamma22 | gamma28 | linear; empty / unknown -> *error. `title` is quoted into the TITLE line.
QString lutCubeText(const PrimaryGrade& g, int size, const QString& inputSpace, const QString& title, QString* error);

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
