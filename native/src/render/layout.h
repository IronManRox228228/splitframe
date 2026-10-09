#pragma once

// The CPU half of compositing: which items are visible, where they go and how they look. Pure
// functions over the project model (ports of packages/renderer: compositor.ts, keyframes.ts,
// captions.ts, effects.ts), so they are testable without a GPU.

#include "core/schema.h"

#include <QSize>
#include <QSizeF>
#include <QTransform>

#include <array>
#include <optional>
#include <vector>

namespace sf::render {

// Items on screen at `frame`, bottom layer first: tracks are stored top-first, so they are walked in
// reverse; hidden tracks are skipped; within a track by start frame (ties by id).
std::vector<const Item*> activeItems(const TimelineDoc& doc, Frame frame);

// keyframes.ts
double resolveKeyframes(const KeyframeList& kfs, Frame localFrame); // kfs must be non-empty
// Keyframed value of "transform.x" etc. at an absolute frame, else the item's static value.
double resolveProperty(const Item& item, const QString& path, Frame frame);
// Opacity with fade in/out and keyframes, clamped to 0..1.
double opacityAt(const Item& item, Frame frame);

// captions.ts
Ms captionClockMs(Frame frame, Frame itemStart, double fps);
struct CaptionCard {
  std::vector<const TranscriptWord*> words;
  int activeIdx = -1;     // index into the item's words, -1 between words
  qsizetype firstWord = 0; // index of words[0] in the item's words
};
std::optional<CaptionCard> captionCardAt(const std::vector<TranscriptWord>& words, Ms timeMs, std::int64_t maxWordsPerCard);

// Where a drawable of size `content` (already upright) lands: fitted into the canvas ("contain"),
// then scaled, rotated and moved by the item's transform. Canvas space is y-down pixels.
struct Placement {
  QPointF center;  // canvas pixels
  QSizeF size;     // fitted size before the item's scale
  double scaleX = 1;
  double scaleY = 1;
  double rotationDeg = 0; // clockwise
  // unit square [0,1]^2 -> canvas pixels
  QTransform unitToCanvas() const;
};
Placement placeMedia(const Item& item, Frame frame, QSizeF content, QSize canvas);

// Rows of an RGB -> RGB affine map: out_i = r[i][0]*R + r[i][1]*G + r[i][2]*B + r[i][3]
using ColorMatrix = std::array<std::array<float, 4>, 3>;
inline constexpr ColorMatrix kIdentityColor = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
// The effect stack as one colour matrix (brightness, contrast, saturation, hueRotate, grayscale,
// sepia). Blur has no colour-matrix form and is ignored (reported through *hasBlur).
ColorMatrix effectsToColorMatrix(const std::vector<Effect>& effects, bool* hasBlur = nullptr);

// Samples (the planes' raw normalised values) -> display RGB, in the stream's matrix and range.
// Rows as above; columns are Y, U(Cb), V(Cr), offset. bitDepth 10 = P010 (value in the top 10 bits).
ColorMatrix yuvToRgb(double kr, double kb, bool fullRange, int bitDepth);

// Source texture coords from display coords: src = (x[0]*u + x[1]*v + x[2], y[0]*u + y[1]*v + y[2]).
// `rotation` is MediaInfo::rotation: degrees clockwise to turn the stored frame upright.
struct UvMap {
  std::array<float, 3> x{1, 0, 0};
  std::array<float, 3> y{0, 1, 0};
};
UvMap uvForRotation(int rotation);

} // namespace sf::render
