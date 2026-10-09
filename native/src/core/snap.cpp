#include "core/snap.h"

#include "core/timeline_doc.h"

#include <algorithm>
#include <cmath>

namespace sf {

std::vector<SnapCandidate> getSnapCandidates(const TimelineDoc& doc, const SnapOptions& opts) {
  std::vector<SnapCandidate> out{{0, SnapCandidate::Kind::Zero, std::nullopt}};
  if (opts.includeItemEdges) {
    for (const Track& track : doc.tracks) {
      if (track.kind == TrackKind::Audio) continue; // visual edges are what you snap against
      for (const Item* item : itemsOnTrack(doc, track.id)) {
        if (std::find(opts.excludeItemIds.begin(), opts.excludeItemIds.end(), item->id) != opts.excludeItemIds.end()) continue;
        out.push_back({static_cast<double>(item->startFrame), SnapCandidate::Kind::ItemEdge, item->id});
        out.push_back({static_cast<double>(itemEnd(*item)), SnapCandidate::Kind::ItemEdge, item->id});
      }
    }
  }
  if (opts.includeMarkers)
    for (const Marker& m : doc.markers) out.push_back({static_cast<double>(m.frame), SnapCandidate::Kind::Marker, m.id});
  if (opts.includePlayhead && *opts.includePlayhead != 0) out.push_back({*opts.includePlayhead, SnapCandidate::Kind::Playhead, std::nullopt});
  out.insert(out.end(), opts.extra.begin(), opts.extra.end());
  if (opts.gridFrames && *opts.gridFrames > 1) {
    const double end = std::max(static_cast<double>(docDurationFrames(doc)), 1.0) + *opts.gridFrames;
    for (double f = 0; f <= end; f += *opts.gridFrames) out.push_back({f, SnapCandidate::Kind::Grid, std::nullopt});
  }
  return out;
}

std::optional<SnapResult> snapFrame(double frame, const std::vector<SnapCandidate>& candidates, double thresholdFrames) {
  std::optional<SnapResult> best;
  for (const SnapCandidate& c : candidates) {
    const double delta = c.frame - frame;
    if (std::fabs(delta) > thresholdFrames) continue;
    if (!best || std::fabs(delta) < std::fabs(best->delta)) best = SnapResult{c.frame, c, delta};
  }
  return best;
}

double snapItemStart(const TimelineDoc& doc, const QString& itemId, double proposedStart, double thresholdFrames,
                     const SnapOptions& opts) {
  const Item* item = getItem(doc, itemId);
  if (!item) return proposedStart;
  SnapOptions withSelf = opts;
  withSelf.excludeItemIds.push_back(itemId);
  const auto candidates = getSnapCandidates(doc, withSelf);
  const auto inSnap = snapFrame(proposedStart, candidates, thresholdFrames);
  const double duration = static_cast<double>(item->durationFrames);
  const auto outSnap = snapFrame(proposedStart + duration, candidates, thresholdFrames);
  if (inSnap && (!outSnap || std::fabs(inSnap->delta) <= std::fabs(outSnap->delta))) return std::max(0.0, inSnap->frame);
  if (outSnap) return std::max(0.0, outSnap->frame - duration);
  return proposedStart;
}

} // namespace sf
