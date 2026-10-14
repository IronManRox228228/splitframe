#pragma once

// Reads pixels for colour measurement through the same offscreen path as export and frame.render: software
// decode, the compositor, linear working-space readback plus the display-referred picture.

#include "render/colour_analysis.h"
#include "render/media_provider.h"
#include "render/offscreen.h"

#include <vector>

namespace sf::render {

// A copy of `doc` holding only `itemId` (and optionally without its primary correction), so a measurement is of
// that clip alone. The item's own grade is kept unless `keepPrimaries` is false (input transform and LUT stay).
TimelineDoc isolateItem(const TimelineDoc& doc, const QString& itemId, bool keepPrimaries);
// `count` frames spread evenly through the item's span (centres of equal slices).
std::vector<Frame> sampleFrames(const Item& item, int count);

struct Capture {
  std::vector<PixelSet> frames;
  QString error;
  bool ok() const { return error.isEmpty() && !frames.empty(); }
};
// Renders each frame of `doc`. `isolated` marks a doc made by isolateItem: pixels the item does not cover (letterbox)
// are found by rendering over black and over white and are left out.
Capture captureFrames(OffscreenRenderer& renderer, const TimelineDoc& doc, const AssetTable& assets, const std::vector<Frame>& frames, bool isolated);

} // namespace sf::render
