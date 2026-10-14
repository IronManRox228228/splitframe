#include "engine/timeline_analysis.h"

#include "core/timeline_doc.h"

#include <algorithm>
#include <cmath>

namespace sf::engine::tl {

std::vector<Shot> shotsOnTrack(const TimelineDoc& doc, const QString& trackId) {
  std::vector<Shot> shots;
  for (const Item& it : doc.items)
    if (it.trackId == trackId && (it.type() == ItemType::Video || it.type() == ItemType::Image)) shots.push_back({it.id, it.startFrame, itemEnd(it)});
  std::sort(shots.begin(), shots.end(), [](const Shot& a, const Shot& b) { return a.start != b.start ? a.start < b.start : a.itemId < b.itemId; });
  return shots;
}

QString primaryVideoTrack(const TimelineDoc& doc) {
  QString best;
  int bestCount = 0;
  for (const Track& t : doc.tracks) {
    if (t.kind != TrackKind::Video) continue;
    const int n = static_cast<int>(shotsOnTrack(doc, t.id).size());
    if (n > bestCount) {
      best = t.id;
      bestCount = n;
    }
  }
  return best;
}

// ---------------------------------------------------------------- stats

ShotStats computeStats(const std::vector<Shot>& shots, double fps, Frame from, Frame to) {
  ShotStats s;
  s.from = from;
  if (shots.empty() || fps <= 0) return s;
  if (to <= 0) {
    to = 0;
    for (const Shot& sh : shots) to = std::max(to, sh.end);
  }
  s.to = to;
  s.durationSec = static_cast<double>(std::max<Frame>(0, to - from)) / fps;

  std::vector<double> lens;
  std::vector<ShortShot> all;
  for (size_t i = 0; i < shots.size(); ++i) {
    const Shot& sh = shots[i];
    if (sh.end <= from || sh.start >= to) continue;
    const Frame len = std::min(sh.end, to) - std::max(sh.start, from);
    lens.push_back(static_cast<double>(len) / fps);
    all.push_back({sh.itemId, std::max(sh.start, from), len});
    if (i > 0 && sh.start > from && sh.start < to) s.cuts.push_back(sh.start);
  }
  s.shotCount = static_cast<int>(lens.size());
  if (lens.empty()) return s;
  double sum = 0;
  for (double l : lens) sum += l;
  s.aslSec = sum / static_cast<double>(lens.size());
  double var = 0;
  for (double l : lens) var += (l - s.aslSec) * (l - s.aslSec);
  s.stdevSec = std::sqrt(var / static_cast<double>(lens.size()));
  std::vector<double> sorted = lens;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  s.medianSec = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
  s.minSec = sorted.front();
  s.maxSec = sorted.back();
  std::stable_sort(all.begin(), all.end(), [](const ShortShot& a, const ShortShot& b) { return a.length < b.length; });
  for (size_t i = 0; i < all.size() && i < 5; ++i) s.shortest.push_back(all[i]);

  const double window = 10.0 * fps;
  const int bins = std::max(1, static_cast<int>(std::ceil(static_cast<double>(to - from) / window)));
  s.cutsPer10s.assign(static_cast<size_t>(bins), 0);
  for (Frame c : s.cuts) {
    const int b = std::clamp(static_cast<int>(static_cast<double>(c - from) / window), 0, bins - 1);
    ++s.cutsPer10s[static_cast<size_t>(b)];
  }
  if (!s.cuts.empty()) s.firstCutSec = static_cast<double>(s.cuts.front() - from) / fps;
  if (s.durationSec > 0) s.cutsPerMinute = static_cast<double>(s.cuts.size()) / s.durationSec * 60.0;
  return s;
}

// ---------------------------------------------------------------- beat alignment

std::optional<Frame> nearestOnGrid(const std::vector<Frame>& grid, Frame frame) {
  if (grid.empty()) return std::nullopt;
  const auto it = std::lower_bound(grid.begin(), grid.end(), frame);
  if (it == grid.begin()) return *it;
  if (it == grid.end()) return grid.back();
  const Frame hi = *it, lo = *(it - 1);
  return frame - lo <= hi - frame ? lo : hi; // earlier beat on a tie
}

BeatAlignment alignment(const std::vector<Frame>& cuts, const std::vector<Frame>& grid, double fps, Frame tolerance) {
  BeatAlignment a;
  a.tolerance = tolerance;
  a.cuts = static_cast<int>(cuts.size());
  if (grid.empty() || cuts.empty()) {
    a.valid = !grid.empty();
    return a;
  }
  a.valid = true;
  double sum = 0, sumAbs = 0;
  for (Frame c : cuts) {
    const Frame beat = *nearestOnGrid(grid, c);
    a.perCut.push_back({c, beat, beat - c});
    if (std::llabs(beat - c) <= tolerance) ++a.within;
    sum += static_cast<double>(c - beat);
    sumAbs += static_cast<double>(std::llabs(c - beat));
  }
  const double n = static_cast<double>(cuts.size());
  a.withinPct = 100.0 * a.within / n;
  a.meanOffsetMs = sum / n / fps * 1000.0;
  a.meanAbsOffsetMs = sumAbs / n / fps * 1000.0;
  return a;
}

// ---------------------------------------------------------------- speech

const FrameSpan* insideWord(const std::vector<FrameSpan>& spans, Frame frame) {
  for (const FrameSpan& s : spans)
    if (frame > s.start && frame < s.end) return &s;
  return nullptr;
}

std::optional<Frame> nearestGapFrame(const std::vector<FrameSpan>& spans, Frame frame, Frame maxShift) {
  const FrameSpan* s = insideWord(spans, frame);
  if (!s) return frame;
  const Frame toStart = frame - s->start, toEnd = s->end - frame;
  const bool startOk = toStart <= maxShift && s->start >= 0, endOk = toEnd <= maxShift;
  if (startOk && (!endOk || toStart <= toEnd)) return s->start;
  if (endOk) return s->end;
  return std::nullopt;
}

// ---------------------------------------------------------------- snapping

namespace {

struct ItemState {
  Frame start = 0, end = 0, sourceIn = 0;
  double speed = 1;
  QString assetId;
  bool media = false;
};

Frame ceilDiv(Frame frames, double speed) { return static_cast<Frame>(std::ceil(static_cast<double>(frames) * (speed > 0 ? speed : 1.0))); }

} // namespace

SnapPlan planSnap(const SnapInput& in) {
  SnapPlan plan;
  if (!in.doc) return plan;
  const std::vector<Shot> shots = shotsOnTrack(*in.doc, in.trackId);
  std::map<QString, ItemState> state;
  for (const Shot& s : shots) {
    const Item* it = getItem(*in.doc, s.itemId);
    ItemState st;
    st.start = it->startFrame;
    st.end = itemEnd(*it);
    st.sourceIn = it->sourceInFrame.value_or(0);
    st.speed = it->speed > 0 ? it->speed : 1.0;
    st.media = it->type() == ItemType::Video;
    if (it->assetId) st.assetId = *it->assetId;
    state[s.itemId] = st;
  }

  for (size_t i = 1; i < shots.size(); ++i) {
    const Shot &left = shots[i - 1], &right = shots[i];
    if (!in.itemIds.empty() && std::find(in.itemIds.begin(), in.itemIds.end(), right.itemId) == in.itemIds.end()) continue;
    ++plan.considered;
    ItemState &L = state[left.itemId], &R = state[right.itemId];
    SnapSkip skip{left.itemId, right.itemId, R.start, {}, {}, std::nullopt};
    if (L.end != R.start) {
      skip.reason = QStringLiteral("not_abutting");
      skip.detail = QStringLiteral("the two shots %1 by %2 frames: close the gap or overlap first").arg(L.end < R.start ? QStringLiteral("are apart") : QStringLiteral("overlap")).arg(std::llabs(R.start - L.end));
      plan.skipped.push_back(skip);
      continue;
    }
    const Frame cut = R.start;
    const std::optional<Frame> target = nearestOnGrid(in.grid, cut);
    if (!target) {
      skip.reason = QStringLiteral("no_grid");
      skip.detail = QStringLiteral("no beats to snap to");
      plan.skipped.push_back(skip);
      continue;
    }
    const Frame shift = *target - cut;
    if (shift == 0) {
      plan.alreadyOnGrid.push_back(cut);
      continue;
    }
    if (std::llabs(shift) > in.maxShift) {
      skip.reason = QStringLiteral("too_far");
      skip.detail = QStringLiteral("the nearest target (frame %1) is %2 frames away, more than maxShiftFrames %3").arg(*target).arg(std::llabs(shift)).arg(in.maxShift);
      plan.skipped.push_back(skip);
      continue;
    }
    const FrameSpan* word = in.allowWordClip ? nullptr : insideWord(in.speech, *target);
    if (word) {
      skip.reason = QStringLiteral("clips_word");
      skip.detail = QStringLiteral("frame %1 is inside speech (%2..%3): the cut would clip a word").arg(*target).arg(word->start).arg(word->end);
      const std::optional<Frame> gap = nearestGapFrame(in.speech, *target, in.maxShift * 2);
      if (gap && *gap != *target && std::llabs(*gap - cut) <= in.maxShift) skip.suggestedFrame = gap;
      plan.skipped.push_back(skip);
      continue;
    }
    if (*target - L.start < in.minShotFrames || R.end - *target < in.minShotFrames) {
      skip.reason = QStringLiteral("too_short");
      skip.detail = QStringLiteral("the move would leave a shot under %1 frames").arg(in.minShotFrames);
      plan.skipped.push_back(skip);
      continue;
    }
    if (shift > 0 && L.media) { // the left shot plays on: it needs source after its current out point
      const auto total = in.sourceFrames.find(L.assetId);
      if (total != in.sourceFrames.end()) {
        const Frame avail = total->second - (L.sourceIn + static_cast<Frame>(std::llround(static_cast<double>(L.end - L.start) * L.speed)));
        if (avail < ceilDiv(shift, L.speed)) {
          skip.reason = QStringLiteral("exceeds_handles");
          skip.detail = QStringLiteral("the shot before the cut has %1 source frames left after its out point, the move needs %2").arg(std::max<Frame>(0, avail)).arg(ceilDiv(shift, L.speed));
          plan.skipped.push_back(skip);
          continue;
        }
      }
    }
    if (shift < 0 && R.media) { // the right shot starts earlier: it needs source before its in point
      const Frame need = ceilDiv(-shift, R.speed);
      if (R.sourceIn < need) {
        skip.reason = QStringLiteral("exceeds_handles");
        skip.detail = QStringLiteral("the shot after the cut has %1 source frames before its in point, the move needs %2").arg(std::max<Frame>(0, R.sourceIn)).arg(need);
        plan.skipped.push_back(skip);
        continue;
      }
    }
    SnapMove m;
    m.leftId = left.itemId;
    m.rightId = right.itemId;
    m.from = cut;
    m.to = *target;
    m.shift = shift;
    m.wasInsideWord = insideWord(in.speech, cut) != nullptr;
    plan.moves.push_back(m);
    L.end = *target;
    R.start = *target;
    if (R.media) R.sourceIn += static_cast<Frame>(std::llround(static_cast<double>(shift) * R.speed));
  }
  return plan;
}

// ---------------------------------------------------------------- holes and overlaps

std::vector<Hole> findGaps(const std::vector<Shot>& shots) {
  std::vector<Hole> out;
  if (!shots.empty() && shots.front().start > 0) out.push_back({QString(), shots.front().itemId, 0, shots.front().start});
  for (size_t i = 1; i < shots.size(); ++i)
    if (shots[i].start > shots[i - 1].end) out.push_back({shots[i - 1].itemId, shots[i].itemId, shots[i - 1].end, shots[i].start - shots[i - 1].end});
  return out;
}

std::vector<Hole> findOverlaps(const std::vector<Shot>& shots) {
  std::vector<Hole> out;
  for (size_t i = 1; i < shots.size(); ++i)
    if (shots[i].start < shots[i - 1].end) out.push_back({shots[i - 1].itemId, shots[i].itemId, shots[i].start, shots[i - 1].end - shots[i].start});
  return out;
}

} // namespace sf::engine::tl
