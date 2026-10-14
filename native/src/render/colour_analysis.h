#pragma once

// Deterministic colour measurement ("measure, then decide; never guess") and the proposals built on it.
// Everything here is a pure function of pixel buffers: no GPU, no document, no I/O.
//
// A PixelSet holds the same pixels twice: scene-linear working space (what the grade acts on) and the
// display-referred picture (what a viewer sees, 0..1). Pictures are point-sampled down to <= 480 px on
// the long side first, which keeps percentages unbiased and the cost flat.
//
// Definitions (all luma uses Rec.709 weights 0.2126 / 0.7152 / 0.0722):
//   linear luma      weights applied to linear RGB (1.0 = SDR white, 0.18 = mid grey)
//   display luma     weights applied to the display-referred RGB (what scopes show)
//   exposure         log2(median linear luma / 0.18), stops, metered over pixels not crushed (all pixels when < 1% remain)
//   contrast         log2(p99 / p1) of linear luma, both floored at 0.001, stops; rms = std-dev of display luma
//   crushed          display value <= 2/255 (per channel, and display luma for "all"), percent of pixels
//   clipped          display value >= 253/255 likewise
//   cast             median over near-neutral pixels (display luma 0.15..0.85, linear max-min over max <= 0.55)
//                    of the white-balance model's own ratios: temperature = log2(R/B), tint = -2 log2(G / sqrt(RB)),
//                    i.e. the same units as PrimaryGrade::temperature / tint (render/color.h: wr = 2^(t/2), wb = 2^(-t/2),
//                    wg = 2^(-tint/2)). The correction that neutralises it is the negation.
//   chroma           display max(R,G,B) - min(R,G,B), 0..1; "gamut limit" = HSV saturation >= 0.95 with value >= 0.1
//   skin             Rec.709 vectorscope (Cb = (B'-Y')/1.8556, Cr = (R'-Y')/1.5748 on display values): hue =
//                    atan2(Cr, Cb) in degrees, the skin-tone line is 123 degrees. Pixel is skin when hue is in
//                    108..146, chroma magnitude 0.04..0.20, display luma 0.20..0.92 and R' > G' > B'.

#include "core/schema.h"
#include "render/color.h"

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <array>
#include <vector>

namespace sf::render {

inline constexpr int kScopeMaxSide = 480;
inline constexpr int kHistogramBins = 64;
inline constexpr double kSkinLineDeg = 123.0;

struct PixelSet {
  std::vector<float> lin;  // r g b, scene-linear working space
  std::vector<float> disp; // r g b, display-referred 0..1
  size_t size() const { return lin.size() / 3; }
  void append(const PixelSet& other);
};

// Display derived from the linear values with the sRGB OETF, quantised to 8 bit (what the default delivery gives).
PixelSet pixelsFromLinear(const std::vector<float>& linearRgb);
// `linear`: Format_RGBA32FPx4_Premultiplied (OffscreenRenderer::renderLinear); `display`: the same canvas as
// RGBA8888 / RGBA64 (may be null: derived from the linear values). Point-samples to <= maxSide, skips pixels
// whose `coverage` entry (0/1 per pixel of the full-size image, may be empty) is 0 or whose alpha is < 0.99.
PixelSet pixelsFromImages(const QImage& linear, const QImage* display, int maxSide = kScopeMaxSide, const std::vector<unsigned char>* coverage = nullptr);
// Every `stride`-th pixel so that at most maxPixels remain.
PixelSet subsample(const PixelSet& in, size_t maxPixels);
// The grade applied on the linear values (the CPU twin of the shader), display re-derived like pixelsFromLinear.
PixelSet gradePixels(const PixelSet& in, const PrimaryGrade& grade);

struct ChannelPct {
  double r = 0, g = 0, b = 0, all = 0;
};

struct ColourMetrics {
  bool valid = false;
  int frames = 1;
  qint64 pixels = 0;
  // luma (linear)
  double lumaMean = 0, lumaMedian = 0, p1 = 0, p5 = 0, p50 = 0, p95 = 0, p99 = 0;
  std::array<double, kHistogramBins> histogram{}; // fraction of pixels per bin of display luma, bin 0 = black
  double exposureStops = 0;                       // log2(median / 0.18)
  double contrastStops = 0, rmsContrast = 0;
  ChannelPct crushedPct, clippedPct;
  // colour cast of near-neutral pixels, in PrimaryGrade units
  double castTemperature = 0, castTint = 0, neutralPct = 0;
  // saturation
  double chromaMean = 0, chromaP95 = 0, gamutLimitPct = 0;
  // skin
  double skinPct = 0, skinHueDeg = 0, skinOffsetDeg = 0;
  // log2 statistics (floor 2^-10) used for shot matching: per channel and of the luma
  std::array<double, 3> logMean{}, logStd{};
  double lumaLogMean = 0, lumaLogStd = 0;
  // across sampled frames (equal to the pooled value for one frame)
  double exposureMin = 0, exposureMax = 0, crushedMin = 0, crushedMax = 0, clippedMin = 0, clippedMax = 0;

  QJsonObject toJson() const;
  // The fields colour.check needs (exposure, crushed, clipped, skin, contrast, cast); the rest stays default.
  static ColourMetrics fromJson(const QJsonObject& o);
};

// The near-neutral pixels (the cast's selection) of a picture, 1 per selected pixel. Grading changes which pixels qualify, so a
// before / after cast comparison must measure BOTH on the same selection: take the mask of the before (or ungraded) picture and pass
// it as `castMask` when analysing the after picture. Only the cast / neutralPct fields use it; a mask of another size is ignored.
std::vector<unsigned char> neutralMask(const PixelSet& pixels);
std::vector<std::vector<unsigned char>> neutralMasks(const std::vector<PixelSet>& frames);
ColourMetrics analyze(const PixelSet& pixels, const std::vector<unsigned char>* castMask = nullptr);
// Pools the pixels of every frame (percentiles are over all of them) and records min / max across frames. castMasks: one per frame, or null.
ColourMetrics analyzeFrames(const std::vector<PixelSet>& frames, const std::vector<std::vector<unsigned char>>* castMasks = nullptr);

// ---- proposals ----

// How bright the scene is meant to be. low / high keep the shot's key: exposure moves at most kKeyedExposureStops either way and aims at the
// key's own median (kLowKeyMedian / kHighKeyMedian), not mid grey. auto classifies from the measured luma distribution (detectSceneKey).
enum class SceneKey { Auto, Low, Normal, High };
inline constexpr double kKeyedExposureStops = 0.5;
inline constexpr double kLowKeyMedian = 0.09;  // linear, one stop under mid grey
inline constexpr double kHighKeyMedian = 0.36; // linear, one stop over
struct SceneKeyReport {
  QString requested, detected, reason;
};
// low: median at least a stop under grey (0.09) with real highlights (p99 >= 0.4, or 0.2 % or more of the pixels at display luma >= 0.75): a
// night shot of lights, not an underexposed one. high: median at least a stop over grey (0.36) with lifted blacks (p5 >= 0.05) and under 1 %
// clipped. Everything else, including a dark picture with no highlights at all, is normal.
SceneKey detectSceneKey(const ColourMetrics& m, QString* reason = nullptr);
QString sceneKeyName(SceneKey k);

struct AutoBalanceOptions {
  double exposureTarget = 0.18;   // linear median luma to aim for
  double toleranceStops = 0.25;   // within this many stops of the target: leave exposure alone
  double maxExposureStops = 2.0;  // |exposure| never exceeds this
  double maxIncreasePct = 1.0;    // crushed / clipped percent may not grow by more than this (percentage points)
  bool whiteBalance = true;
  SceneKey sceneKey = SceneKey::Auto;
};
struct Proposal {
  PrimaryGrade primaries;
  ColourMetrics before, predicted; // predicted = the CPU twin applied to the pixels
  QStringList notes;               // what was limited and why
  SceneKeyReport sceneKey;         // autoBalance only
  double error = 0;               // match only: RMS residual in stops (see MatchResidual)
  QJsonObject residual;            // match only
  int iterations = 0;
  bool plausible = true;           // match only: false when reaching the reference took grade values no colourist would use
};
// `source` must be the picture WITHOUT any primary correction: the proposal is an absolute grade.
Proposal proposeAutoBalance(const PixelSet& source, const AutoBalanceOptions& options = {});
// Reinhard-style statistics transfer: per-channel log2 mean / std of the target are moved onto the reference's by
// exposure (shift), contrast (std ratio around the pivot), temperature / tint (opponent R-B and G-(R+B)/2 means),
// saturation (mean chroma ratio) and, if clamps leave a residual, per-channel gain. Iterated against the CPU twin.
// A low residual says the statistics match, not that the result looks right: when the grade needs more than 2 stops of
// exposure, contrast outside 0.6..1.6, a full-scale white balance or a channel gain outside 0.5..2, `plausible` is
// false and a note names the values (the two shots are probably different scenes).
Proposal proposeMatch(const PixelSet& target, const ColourMetrics& reference);

struct CheckThresholds {
  double blacksCrushedIncrease = 2.0;     // percentage points of display luma <= 2/255
  double highlightsClippedIncrease = 2.0; // percentage points of display luma >= 253/255
  double skinHueShiftDeg = 6.0;           // only checked when skin covers more than skinMinPct
  double skinMinPct = 2.0;
  double exposureShiftStops = 1.0;        // the requested change of the median; more than this is a violation
};
struct Violation {
  QString code;
  QString message;
  double value = 0, limit = 0;
  QJsonObject toJson() const;
};
std::vector<Violation> checkColour(const ColourMetrics& before, const ColourMetrics& after, const CheckThresholds& t = {});
CheckThresholds thresholdsFromJson(const QJsonObject& o);

QJsonArray violationsJson(const std::vector<Violation>& v);

} // namespace sf::render
