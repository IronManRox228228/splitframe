#pragma once

// Internal: decoded AVFrame -> RGBA8888 QImage, honouring the frame's own colour matrix and range.

#include "media/ffmpeg.h"

#include <QImage>
#include <QSize>

namespace sf::media {

class FrameConverter {
public:
  FrameConverter();

  // `src` is system-memory pixels (hardware frames are downloaded first). Scales to `out` when it is
  // non-empty, otherwise keeps the source size. Not thread-safe: one converter per decoder.
  // Takes src non-const only to fill in colour properties the stream left unspecified.
  QImage toRgba(AVFrame* src, QSize out = {});

private:
  av::SwsPtr sws_;
};

} // namespace sf::media
