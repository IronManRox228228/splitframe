#pragma once

// Items that are drawn rather than decoded: text, captions, shapes and the stand-in cards
// (media unavailable, motion graphic). Ports of drawTextBlock / drawCaption / drawShape and the
// placeholders in packages/renderer/src/compositor.ts, painted with QPainter into an image that
// covers just the item's bounding box, so a small title costs a small upload.

#include "core/schema.h"

#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>

#include <optional>

namespace sf::render {

struct RasterLayer {
  QImage image;   // Format_RGBA8888_Premultiplied (the byte order QRhi uploads as-is)
  QPoint origin;  // top-left in canvas pixels
  QString key;    // identical key = identical pixels; lets the compositor skip re-uploading
};

// Text, caption, shape and motionGraphic items; nullopt when there is nothing to show (an empty
// caption card, a fully transparent item, off-canvas). Placement follows the reference exactly:
// text and shapes at canvas centre + transform, captions at canvas width/2 and `placementY` of the
// height. Opacity is NOT applied here; the compositor multiplies it in.
std::optional<RasterLayer> rasterizeItem(const TimelineDoc& doc, const Item& item, Frame frame, QSize canvas);

// "Media unavailable" card for a video/image item whose file can't be read.
std::optional<RasterLayer> rasterizeMissingMedia(const Item& item, Frame frame, QSize canvas);

} // namespace sf::render
