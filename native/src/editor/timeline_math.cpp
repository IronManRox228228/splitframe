#include "editor/timeline_math.h"

#include "core/timeline_doc.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace sf::editor {

int rowHeightForKind(TrackKind kind) {
  switch (kind) {
  case TrackKind::Video: return 64;
  case TrackKind::Audio: return 48;
  default: return 44;
  }
}

std::vector<RowLayout> trackLayout(const std::vector<Track>& tracks) {
  std::vector<RowLayout> out;
  out.reserve(tracks.size());
  int top = 0;
  for (const Track& t : tracks) {
    const int h = rowHeightForKind(t.kind);
    out.push_back({top, h});
    top += h;
  }
  return out;
}

int totalRowsHeight(const std::vector<RowLayout>& layout) { return layout.empty() ? 0 : layout.back().top + layout.back().height; }

int rowIndexAtY(const std::vector<RowLayout>& layout, double y) {
  if (layout.empty()) return -1;
  for (size_t i = 0; i < layout.size(); ++i) {
    if (y < layout[i].top + layout[i].height) return static_cast<int>(i);
  }
  return static_cast<int>(layout.size()) - 1;
}

double clampPxPerFrame(double px) { return std::clamp(px, kMinPxPerFrame, kMaxPxPerFrame); }

Frame rulerInterval(double pxPerFrame, double fps, double minPx) {
  static constexpr int kSecondSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
  std::vector<Frame> candidates;
  for (const int f : {1, 2, 5, 10}) {
    if (f < fps) candidates.push_back(f);
  }
  for (const int s : kSecondSteps) candidates.push_back(jsRound(s * fps));
  for (const Frame f : candidates) {
    if (static_cast<double>(f) * pxPerFrame >= minPx) return f;
  }
  return candidates.back();
}

int rulerSubdivisions(Frame interval, double pxPerFrame) {
  for (const int n : {5, 4, 2}) {
    if (interval % n == 0 && static_cast<double>(interval) / n * pxPerFrame >= 8) return n;
  }
  return 0;
}

double trimHandleWidth(double clipWidthPx) {
  if (clipWidthPx < 20) return 0;
  return std::min(8.0, clipWidthPx / 3);
}

QString formatDurationBadge(Frame frames, double fps) {
  const double secs = static_cast<double>(std::max<Frame>(0, frames)) / fps;
  return QStringLiteral("%1s").arg(secs, 0, 'f', secs >= 10 ? 1 : 2);
}

std::vector<Frame> clipEdges(const TimelineDoc& doc) {
  std::set<Frame> edges;
  for (const Item& i : doc.items) {
    edges.insert(i.startFrame);
    edges.insert(itemEnd(i));
  }
  return {edges.begin(), edges.end()};
}

std::optional<Frame> neighborEdge(const std::vector<Frame>& edges, Frame from, int dir) {
  if (dir > 0) {
    const auto it = std::upper_bound(edges.begin(), edges.end(), from);
    if (it != edges.end()) return *it;
    return std::nullopt;
  }
  const auto it = std::lower_bound(edges.begin(), edges.end(), from);
  if (it != edges.begin()) return *(it - 1);
  return std::nullopt;
}

double scrollForZoom(double anchorFrame, double anchorViewX, double newPxPerFrame) {
  return std::max(0.0, anchorFrame * newPxPerFrame - anchorViewX);
}

} // namespace sf::editor
