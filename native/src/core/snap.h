#pragma once

#include "core/schema.h"

#include <optional>
#include <vector>

// Edge/marker/playhead/grid snapping for timeline drags (packages/editor-core/src/snap.ts).
// Frames are doubles because a drag position need not be whole; item edges always are.
namespace sf {

struct SnapCandidate {
  enum class Kind { ItemEdge, Marker, Playhead, Grid, Zero };
  double frame = 0;
  Kind kind = Kind::Zero;
  std::optional<QString> source;
};

struct SnapOptions {
  bool includeItemEdges = true;
  bool includeMarkers = true;
  // the live playhead position as an extra candidate; like the TS truthiness test, 0 adds nothing
  std::optional<double> includePlayhead;
  std::optional<double> gridFrames;
  std::vector<QString> excludeItemIds;
  // extra candidates supplied by the UI
  std::vector<SnapCandidate> extra;
};

std::vector<SnapCandidate> getSnapCandidates(const TimelineDoc& doc, const SnapOptions& opts = {});

struct SnapResult {
  double frame = 0;
  SnapCandidate candidate;
  double delta = 0; // distance in frames from the original position
};

// Snap `frame` to the nearest candidate within `thresholdFrames`; ties keep the earlier candidate.
std::optional<SnapResult> snapFrame(double frame, const std::vector<SnapCandidate>& candidates, double thresholdFrames);

// Snap a moving item so its in OR out edge lands on a candidate (whichever is closer). Returns the
// adjusted start frame.
double snapItemStart(const TimelineDoc& doc, const QString& itemId, double proposedStart, double thresholdFrames,
                     const SnapOptions& opts = {});

} // namespace sf
