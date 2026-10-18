#pragma once

// Deterministic assembly solver: rough cut from shots + music + pacing profile.

#include <QString>
#include <QStringList>
#include <vector>

namespace sf::engine {

struct ShotCandidate {          // one usable stretch of one asset
  QString assetId;
  double startSec = 0, endSec = 0;   // usable range in the source
  double sharpness = 0, lumaAvg = 128, clippedPct = 0, motion = 0;
  bool freeze = false, black = false;
};

struct PacingTarget {
  double aslMin = 2, aslMax = 4, minShot = 0.5, maxShot = 8, hookMax = 2.5;
  int cutsOnBeatMinPct = 60;
};

struct MusicGrid {
  double durationSec = 0;
  std::vector<double> beats, downbeats;
  struct Sec {
    double start = 0, end = 0, relEnergy = 0.5;
    QString label;
  };
  std::vector<Sec> sections;
};

struct PlacedShot {
  QString assetId;
  double srcInSec = 0, srcOutSec = 0;
  double timelineStartSec = 0, timelineEndSec = 0;
  QString reason;
};

struct AssemblyPlan {
  bool ok = false;
  QString error;
  std::vector<PlacedShot> shots;
  double totalSec = 0;
  int cutsOnBeat = 0;
  int cuts = 0;
  double aslSec = 0;
  QStringList notes;
  QStringList warnings;
};

AssemblyPlan planAssembly(const std::vector<ShotCandidate>& pool,
                         const MusicGrid& music,
                         const PacingTarget& pace,
                         double targetSec /* <=0: music length */,
                         quint32 seed);

} // namespace sf::engine
