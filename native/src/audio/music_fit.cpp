#include "audio/music_fit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace sf::audio {

namespace {

double snapDownbeat(const std::vector<double>& grid, double t) {
  if (grid.empty()) return t;
  double best = grid.front();
  double bestDiff = std::fabs(grid.front() - t);
  for (double g : grid) {
    double diff = std::fabs(g - t);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = g;
    }
  }
  return best;
}

bool isSectionBoundary(const MusicStructure& ms, double t) {
  for (const auto& sec : ms.sections) {
    if (std::fabs(sec.startSec - t) <= 0.01) return true;
  }
  return false;
}

const MusicSection* sectionAt(const MusicStructure& ms, double t) {
  for (const auto& sec : ms.sections) {
    if (t >= sec.startSec - 1e-4 && t < sec.endSec - 1e-4) {
      return &sec;
    }
  }
  if (!ms.sections.empty()) {
    if (t < ms.sections.front().startSec) return &ms.sections.front();
    return &ms.sections.back();
  }
  return nullptr;
}

FitPlan planTrim(const MusicStructure& ms, double T, double D, double bar, double effectiveFade) {
  FitPlan plan;
  plan.ok = true;
  plan.modeUsed = FitMode::Trim;
  plan.segments.push_back({0.0, T});
  plan.totalSec = T;
  plan.fadeOutSec = (std::fabs(T - D) < 1e-4) ? 0.0 : effectiveFade;
  plan.keepsFinalHit = (ms.finalHitSec >= 0.0 && ms.finalHitSec <= T + 1e-4);

  double bestG = -1.0;
  for (double g : ms.downbeats) {
    if (g >= T - bar - 1e-4 && g <= T + 1e-4) {
      if (bestG < 0.0 || g > bestG) {
        bestG = g;
      }
    }
  }
  if (bestG >= 0.0) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "cut lands %.3f s after the downbeat at %.3f", T - bestG, bestG);
    plan.notes.push_back(buf);
  }
  return plan;
}

FitPlan planEdit(const MusicStructure& ms, double availableSec, const FitRequest& req) {
  const double T = req.targetSec;
  const double D = std::min(availableSec, ms.durationSec);
  const double bar = 4.0 * ms.beatPeriodSec;
  const auto& grid = ms.downbeats;
  const double defaultFade = std::clamp(bar, 0.5, 3.0);
  const double effectiveFade = req.fadeOutSec >= 0.0 ? req.fadeOutSec : defaultFade;

  struct Candidate {
    double A = 0;
    double Bs = 0;
    double score = 1e9;
    const MusicSection* secA = nullptr;
    const MusicSection* secB = nullptr;
    double energyDiff = 0;
  };
  std::vector<Candidate> candidates;

  for (double A : grid) {
    if (A < 4.0 * bar - 1e-4) continue;
    double B = A + (D - T);
    double Bs = snapDownbeat(grid, B);

    if (Bs <= A + 1e-4) continue;
    if (Bs > D - 2.0 * bar + 1e-4) continue;
    if (ms.finalHitSec >= 0.0 && Bs > ms.finalHitSec + 1e-4) continue;

    double err = std::fabs(A + (D - Bs) - T);
    double penalty = 0.0;
    if (!isSectionBoundary(ms, A)) penalty += 0.5;
    if (!isSectionBoundary(ms, Bs)) penalty += 0.5;

    const MusicSection* sA = sectionAt(ms, A - 1e-3);
    const MusicSection* sB = sectionAt(ms, Bs + 1e-4);
    double relA = sA ? sA->relEnergy : 0.0;
    double relB = sB ? sB->relEnergy : 0.0;
    double eDiff = std::fabs(relA - relB);

    double totalScore = err + penalty + eDiff;
    candidates.push_back({A, Bs, totalScore, sA, sB, eDiff});
  }

  if (candidates.empty()) {
    FitPlan plan = planTrim(ms, T, D, bar, effectiveFade);
    plan.notes.insert(plan.notes.begin(), "no clean edit point: trim");
    return plan;
  }

  std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) {
    if (std::fabs(x.score - y.score) > 1e-6) return x.score < y.score;
    return x.A < y.A;
  });

  const Candidate& best = candidates.front();
  FitPlan plan;
  plan.ok = true;
  plan.modeUsed = FitMode::Edit;
  plan.segments.push_back({0.0, best.A});

  double tailEnd = D;
  double initialTotal = best.A + (D - best.Bs);
  if (initialTotal > T) {
    tailEnd = D - (initialTotal - T);
  }
  plan.segments.push_back({best.Bs, tailEnd});
  plan.totalSec = best.A + (tailEnd - best.Bs);

  if (std::fabs(tailEnd - D) < 1e-4) {
    plan.fadeOutSec = 0.0;
  } else {
    plan.fadeOutSec = effectiveFade;
  }

  plan.keepsFinalHit = (ms.finalHitSec >= 0.0 && best.Bs <= ms.finalHitSec + 1e-4 && ms.finalHitSec <= tailEnd + 1e-4);

  char note[256];
  std::string labelA = best.secA ? best.secA->label : "unknown";
  std::string labelB = best.secB ? best.secB->label : "unknown";
  std::snprintf(note, sizeof(note), "edit: removed %.3f-%.3f s (%s->%s, energy diff %.2f)",
                best.A, best.Bs, labelA.c_str(), labelB.c_str(), best.energyDiff);
  plan.notes.push_back(note);

  return plan;
}

FitSegment pickLoopSection(const MusicStructure& ms, double D, double bar) {
  const auto& grid = ms.downbeats;
  const MusicSection* best = nullptr;
  int bestBars = -1;

  // 1. Label is "drop" or "main", most bars, tie-break lowest startSec
  for (const auto& sec : ms.sections) {
    if (sec.label == "drop" || sec.label == "main") {
      int bars = (sec.endBar > sec.startBar) ? (sec.endBar - sec.startBar)
                                            : static_cast<int>(std::round((sec.endSec - sec.startSec) / bar));
      if (bars > bestBars) {
        bestBars = bars;
        best = &sec;
      } else if (bars == bestBars && best && sec.startSec < best->startSec) {
        best = &sec;
      }
    }
  }
  if (best) return FitSegment{best->startSec, best->endSec};

  // 2. Longest section not intro or outro, tie-break lowest startSec
  bestBars = -1;
  for (const auto& sec : ms.sections) {
    if (sec.label != "intro" && sec.label != "outro") {
      int bars = (sec.endBar > sec.startBar) ? (sec.endBar - sec.startBar)
                                            : static_cast<int>(std::round((sec.endSec - sec.startSec) / bar));
      if (bars > bestBars) {
        bestBars = bars;
        best = &sec;
      } else if (bars == bestBars && best && sec.startSec < best->startSec) {
        best = &sec;
      }
    }
  }
  if (best) return FitSegment{best->startSec, best->endSec};

  // 3. Whole grid: [grid[0], last downbeat <= D]
  double lastDb = grid.front();
  for (double g : grid) {
    if (g <= D + 1e-4) lastDb = g;
  }
  return FitSegment{grid.front(), lastDb};
}

FitPlan planLoop(const MusicStructure& ms, double availableSec, const FitRequest& req) {
  const double T = req.targetSec;
  const double D = std::min(availableSec, ms.durationSec);
  const double bar = 4.0 * ms.beatPeriodSec;
  const auto& grid = ms.downbeats;
  const double defaultFade = std::clamp(bar, 0.5, 3.0);
  const double effectiveFade = req.fadeOutSec >= 0.0 ? req.fadeOutSec : defaultFade;

  FitSegment L = pickLoopSection(ms, D, bar);
  double loopLen = L.srcEndSec - L.srcStartSec;
  if (loopLen < bar) loopLen = bar;

  // Smallest k >= 0 with L.endSec + k*loopLen + (D - L.startSec) >= T
  int k = 0;
  while (L.srcEndSec + k * loopLen + (D - L.srcStartSec) < T - 1e-4) {
    ++k;
  }

  double initialTotal = L.srcEndSec + k * loopLen + (D - L.srcStartSec);
  double excess = std::max(0.0, initialTotal - T);

  FitPlan plan;
  plan.ok = true;
  plan.modeUsed = FitMode::Loop;

  plan.segments.push_back({0.0, L.srcEndSec});
  for (int i = 0; i < k; ++i) {
    plan.segments.push_back({L.srcStartSec, L.srcEndSec});
  }
  plan.segments.push_back({L.srcStartSec, D});

  if (excess > 1e-4) {
    bool cutsFinalHit = (ms.finalHitSec >= 0.0 && (D - excess < ms.finalHitSec - 1e-4));
    if (!cutsFinalHit) {
      plan.segments.back().srcEndSec = D - excess;
    } else {
      double barExcess = std::floor(excess / bar) * bar;
      size_t segToShorten = (k > 0) ? static_cast<size_t>(k) : 0u;
      double curLen = plan.segments[segToShorten].srcEndSec - plan.segments[segToShorten].srcStartSec;
      double maxShorten = std::max(0.0, std::floor((curLen - bar) / bar) * bar);
      double actualShorten = std::min(barExcess, maxShorten);

      if (actualShorten > 1e-4) {
        double newEnd = plan.segments[segToShorten].srcEndSec - actualShorten;
        newEnd = snapDownbeat(grid, newEnd);
        plan.segments[segToShorten].srcEndSec = newEnd;
      }

      double remainingExcess = excess - actualShorten;
      double tailEnd = D - remainingExcess;
      if (tailEnd - plan.segments.back().srcStartSec < bar) {
        tailEnd = plan.segments.back().srcStartSec + bar;
      }
      plan.segments.back().srcEndSec = tailEnd;

      if (ms.finalHitSec >= 0.0 && tailEnd < ms.finalHitSec - 1e-4) {
        plan.keepsFinalHit = false;
        plan.notes.push_back("final hit lost: remainder <1 bar trimmed from tail");
      }
    }
  }

  double sum = 0.0;
  for (const auto& s : plan.segments) sum += (s.srcEndSec - s.srcStartSec);
  plan.totalSec = sum;

  if (std::fabs(plan.segments.back().srcEndSec - D) < 1e-4) {
    plan.fadeOutSec = 0.0;
  } else {
    plan.fadeOutSec = effectiveFade;
  }

  bool noteLost = false;
  for (const auto& n : plan.notes) {
    if (n == "final hit lost: remainder <1 bar trimmed from tail") {
      noteLost = true;
      break;
    }
  }
  if (!noteLost) {
    const auto& lastSeg = plan.segments.back();
    plan.keepsFinalHit = (ms.finalHitSec >= 0.0 &&
                          lastSeg.srcStartSec <= ms.finalHitSec + 1e-4 &&
                          ms.finalHitSec <= lastSeg.srcEndSec + 1e-4);
  }

  char note[256];
  const MusicSection* secObj = sectionAt(ms, (L.srcStartSec + L.srcEndSec) * 0.5);
  std::string label = secObj ? secObj->label : "section";
  std::snprintf(note, sizeof(note), "loop: repeated section '%s' (%.3f-%.3f s) %d time(s)",
                label.c_str(), L.srcStartSec, L.srcEndSec, k);
  plan.notes.insert(plan.notes.begin(), note);

  return plan;
}

} // namespace

FitPlan planMusicFit(const MusicStructure& ms, double availableSec, const FitRequest& req) {
  const double T = req.targetSec;
  const double D = std::min(availableSec, ms.durationSec);
  const double bar = 4.0 * ms.beatPeriodSec;
  const auto& grid = ms.downbeats;
  const double defaultFade = std::clamp(bar, 0.5, 3.0);
  const double effectiveFade = req.fadeOutSec >= 0.0 ? req.fadeOutSec : defaultFade;

  if (T <= 0.0) {
    return FitPlan{.ok = false, .error = "targetSec must be > 0"};
  }

  const bool gridUsable = ms.valid && ms.bpm > 0.0 && ms.beatPeriodSec > 0.0 && grid.size() >= 8;
  if (gridUsable && T < 4.0 * bar - 1e-4) {
    return FitPlan{.ok = false, .error = "target shorter than 4 bars"};
  }

  if (!gridUsable) {
    if (T <= D + 1e-4) {
      FitPlan plan;
      plan.ok = true;
      plan.modeUsed = FitMode::Trim;
      plan.segments.push_back({0.0, T});
      plan.totalSec = T;
      plan.fadeOutSec = effectiveFade;
      plan.keepsFinalHit = (ms.finalHitSec >= 0.0 && ms.finalHitSec <= T + 1e-4);
      plan.notes.push_back("no beat grid: plain trim + fade");
      return plan;
    } else {
      return FitPlan{.ok = false, .error = "music too short and no beat grid to loop on"};
    }
  }

  if (std::fabs(T - D) <= 0.5 * ms.beatPeriodSec) {
    FitPlan plan;
    plan.ok = true;
    plan.modeUsed = FitMode::Trim;
    plan.segments.push_back({0.0, D});
    plan.totalSec = D;
    plan.fadeOutSec = 0.0;
    plan.keepsFinalHit = (ms.finalHitSec >= 0.0 && ms.finalHitSec <= D + 1e-4);
    plan.notes.push_back("already fits");
    return plan;
  }

  FitMode mode = req.mode;
  if (mode == FitMode::Auto) {
    if (T < D) mode = FitMode::Edit;
    else mode = FitMode::Loop;
  }

  if (mode == FitMode::Trim) {
    return planTrim(ms, T, D, bar, effectiveFade);
  } else if (mode == FitMode::Edit) {
    return planEdit(ms, availableSec, req);
  } else {
    return planLoop(ms, availableSec, req);
  }
}

} // namespace sf::audio
