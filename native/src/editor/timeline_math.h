#pragma once

// Pure timeline geometry and navigation helpers (apps/desktop/src/renderer/lib/timeline-math.ts and
// the edge helpers of shortcuts.ts). Everything is in frames or logical pixels; nothing here knows
// about Qt Quick, so the editing controller and the painted view share it and tests can pin it.

#include "core/schema.h"

#include <QString>

#include <optional>
#include <vector>

namespace sf::editor {

inline constexpr int kRulerHeight = 32;
inline constexpr int kRowPad = 4; // vertical gap above and below a clip inside its row
inline constexpr double kMinPxPerFrame = 0.2;
inline constexpr double kMaxPxPerFrame = 40;
inline constexpr double kDefaultPxPerFrame = 3;

// Video rows are tallest (thumbnails), then audio (waveform), then text and the rest.
int rowHeightForKind(TrackKind kind);

struct RowLayout {
  int top = 0;
  int height = 0;
};
std::vector<RowLayout> trackLayout(const std::vector<Track>& tracks);
int totalRowsHeight(const std::vector<RowLayout>& layout);
// Row under a y offset inside the lanes (clamped to the first/last row); -1 when there are no rows.
int rowIndexAtY(const std::vector<RowLayout>& layout, double y);

double clampPxPerFrame(double px);

// Major ruler tick interval in frames: the smallest "nice" interval that keeps labels >= minPx apart.
Frame rulerInterval(double pxPerFrame, double fps, double minPx = 88);
// Minor ticks per major interval (0 when they would be crowded).
int rulerSubdivisions(Frame interval, double pxPerFrame);

// Hit width of a trim handle: ~8 px, at most a third of the clip, none under ~20 px.
double trimHandleWidth(double clipWidthPx);

// "1.2s": duration badge shown while trimming.
QString formatDurationBadge(Frame frames, double fps);

// Sorted unique clip start/end frames; the nearest one strictly before (-1) / after (+1) `from`.
std::vector<Frame> clipEdges(const TimelineDoc& doc);
std::optional<Frame> neighborEdge(const std::vector<Frame>& edges, Frame from, int dir);

// Scroll offset that keeps `anchorFrame` under `anchorViewX` after the zoom changed.
double scrollForZoom(double anchorFrame, double anchorViewX, double newPxPerFrame);

} // namespace sf::editor
