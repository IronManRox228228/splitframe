#pragma once

// Measured editing: pure functions over a TimelineDoc (no engine, no I/O), so they test on synthetic timelines. Deterministic code
// measures, proposes and verifies; the engine commands (engine/cmd_timeline.cpp) call these, then an LLM may pick between proposals.
//
//   shotsOnTrack / primaryVideoTrack   the shots of the storyline and where its cuts are
//   computeStats                       average shot length, spread, cut rhythm, first cut
//   alignment                          how close the cuts sit to a beat grid
//   insideWord / nearestGapFrame       cuts against speech spans: is a frame inside a word, where is the nearest gap
//   planSnap                           move cuts onto a beat grid with trims of the two adjacent items, within source handles
//   findGaps / findOverlaps            holes and overlaps on a track

#include "core/schema.h"

#include <QString>

#include <map>
#include <optional>
#include <vector>

namespace sf::engine::tl {

struct Shot {
  QString itemId;
  Frame start = 0, end = 0; // end exclusive
  Frame length() const { return end - start; }
};

// Video and image items of one track, ordered by start frame.
std::vector<Shot> shotsOnTrack(const TimelineDoc& doc, const QString& trackId);
// The video-kind track holding the most video / image items (the first one on a tie); empty when there is none.
QString primaryVideoTrack(const TimelineDoc& doc);

// ---------------------------------------------------------------- stats

struct ShortShot {
  QString itemId;
  Frame start = 0, length = 0;
};

struct ShotStats {
  int shotCount = 0;
  std::vector<Frame> cuts; // start frames of every shot after the first
  Frame from = 0, to = 0;
  double durationSec = 0;
  double aslSec = 0, medianSec = 0, stdevSec = 0, minSec = 0, maxSec = 0;
  std::vector<ShortShot> shortest; // up to 5, shortest first
  std::vector<int> cutsPer10s;     // one value per 10 s window from `from`
  double firstCutSec = -1;         // seconds from `from` to the first cut, -1 when there is none
  double cutsPerMinute = 0;
};

// Shots overlapping [from, to) (to <= 0: the end of the last shot), lengths clipped to the range; cuts strictly inside it.
ShotStats computeStats(const std::vector<Shot>& shots, double fps, Frame from = 0, Frame to = 0);

// ---------------------------------------------------------------- beat alignment

struct CutOffset {
  Frame cut = 0, beat = 0;
  Frame offset = 0; // beat - cut: positive when the beat is after the cut
};

struct BeatAlignment {
  bool valid = false; // false: no grid
  int cuts = 0, within = 0;
  double withinPct = 0;
  double meanOffsetMs = 0;    // signed mean of (cut - beat): positive = cuts land late
  double meanAbsOffsetMs = 0;
  Frame tolerance = 2;
  std::vector<CutOffset> perCut;
};

// `grid` sorted ascending. A cut is aligned when its nearest grid frame is within +-tolerance frames.
BeatAlignment alignment(const std::vector<Frame>& cuts, const std::vector<Frame>& grid, double fps, Frame tolerance = 2);
// Nearest grid frame to `frame` (grid sorted); nullopt for an empty grid.
std::optional<Frame> nearestOnGrid(const std::vector<Frame>& grid, Frame frame);

// ---------------------------------------------------------------- speech

struct FrameSpan {
  Frame start = 0, end = 0; // end exclusive
};

// The span (merged, sorted) a cut at `frame` would land inside: start < frame < end. A cut exactly on an edge is fine.
const FrameSpan* insideWord(const std::vector<FrameSpan>& spans, Frame frame);
// `frame` itself when it is in a gap, else the closer edge of the containing span within maxShift (earlier edge on a tie);
// nullopt when neither edge is that near.
std::optional<Frame> nearestGapFrame(const std::vector<FrameSpan>& spans, Frame frame, Frame maxShift);

// ---------------------------------------------------------------- snapping

struct SnapInput {
  const TimelineDoc* doc = nullptr;
  QString trackId;
  std::vector<QString> itemIds;    // cuts at the head of these items; empty: every cut on the track
  std::vector<Frame> grid;         // sorted target frames
  Frame maxShift = 6;
  bool allowWordClip = false;
  std::vector<FrameSpan> speech;   // merged speech spans in timeline frames
  std::map<QString, Frame> sourceFrames; // assetId -> frames of source media at the doc fps; absent = unlimited handles
  Frame minShotFrames = 2;
};

struct SnapMove {
  QString leftId, rightId; // the item that ends at the cut, the item that starts at it
  Frame from = 0, to = 0, shift = 0;
  bool wasInsideWord = false;
};

struct SnapSkip {
  QString leftId, rightId;
  Frame frame = 0;
  QString reason; // too_far | exceeds_handles | clips_word | not_abutting | too_short | no_grid
  QString detail;
  std::optional<Frame> suggestedFrame; // clips_word: the nearest gap target on the grid side, when one exists
};

struct SnapPlan {
  std::vector<SnapMove> moves;      // shift != 0, in timeline order
  std::vector<SnapSkip> skipped;
  std::vector<Frame> alreadyOnGrid; // cuts that need no move
  int considered = 0;
};

SnapPlan planSnap(const SnapInput& in);

// ---------------------------------------------------------------- holes and overlaps

struct Hole {
  QString beforeId, afterId; // beforeId empty for a gap at the start
  Frame frame = 0;           // where the hole starts
  Frame frames = 0;
};

// Gaps between consecutive shots (and before the first shot when it starts after frame 0).
std::vector<Hole> findGaps(const std::vector<Shot>& shots);
// Consecutive shots whose ranges overlap.
std::vector<Hole> findOverlaps(const std::vector<Shot>& shots);

} // namespace sf::engine::tl
