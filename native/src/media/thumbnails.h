#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include <functional>
#include <vector>

namespace sf {

struct Thumbnail {
  double requestedSec = 0; // where along the clip this slot was asked for
  double actualSec = 0;    // the keyframe actually shown (at or before requestedSec)
  QImage image;            // RGBA8888, upright (display rotation applied), fits maxSize
};

// Evenly spaced thumbnails for a timeline strip. Each one is the keyframe nearest before its slot
// (containers that index by decode time may hand back one a few frames after it, within the B-frame delay):
// one seek and one frame decode, never a walk through the GOP, so a strip over a long GOP clip
// costs `count` keyframe decodes however long the clip is. Clips with sparse keyframes therefore
// repeat the same picture across neighbouring slots (the repeats share one QImage). Software
// decode on purpose: a single I-frame is quicker than spinning up a GPU session per clip.
// Runs on the calling thread and touches the disk: call from a worker. `cancel` is polled per slot.
std::vector<Thumbnail> extractThumbnailStrip(const QString& path, int count, QSize maxSize = {160, 90},
                                             const std::function<bool()>& cancel = {}, QString* error = nullptr);

} // namespace sf
