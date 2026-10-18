#pragma once

#include "audio/audio_analysis.h"

#include <string>
#include <vector>

namespace sf::audio {

struct FitSegment {
  double srcStartSec = 0; // seconds from the start of the analysed audio (the item's in point)
  double srcEndSec = 0;   // exclusive
};

enum class FitMode { Auto, Trim, Edit, Loop };

struct FitRequest {
  double targetSec = 0;    // wanted total length
  FitMode mode = FitMode::Auto;
  double fadeOutSec = -1;  // -1 = default: one bar, clamped to 0.5..3.0 s
};

struct FitPlan {
  bool ok = false;
  std::string error;           // set when !ok
  FitMode modeUsed = FitMode::Trim;
  std::vector<FitSegment> segments; // played back to back, in order
  double totalSec = 0;         // sum of segment lengths
  double fadeOutSec = 0;       // fade on the last segment's end
  bool keepsFinalHit = false;  // the last segment contains ms.finalHitSec
  std::vector<std::string> notes; // one line per decision, human readable
};

FitPlan planMusicFit(const MusicStructure& ms, double availableSec, const FitRequest& req);

} // namespace sf::audio
