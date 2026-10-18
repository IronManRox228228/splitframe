// Deterministic assembly solver: rough cut from shots + music + pacing profile.

#include "engine/assembly.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <map>

namespace sf::engine {

namespace {

double normVal(double val, double minVal, double maxVal) {
  if (std::abs(maxVal - minVal) < 1e-9) return 0.5;
  return (val - minVal) / (maxVal - minVal);
}

double sectionTargetLen(double t, bool isFirst, const MusicGrid& music, const PacingTarget& pace) {
  double e = 0.5;
  if (!music.sections.empty()) {
    bool found = false;
    for (const auto& sec : music.sections) {
      if (t >= sec.start - 1e-6 && t < sec.end - 1e-6) {
        e = std::clamp(sec.relEnergy, 0.0, 1.0);
        found = true;
        break;
      }
    }
    if (!found) {
      e = std::clamp(music.sections.back().relEnergy, 0.0, 1.0);
    }
  }
  double len = pace.aslMax - e * (pace.aslMax - pace.aslMin);
  if (isFirst) {
    len = std::min(len, pace.hookMax);
  }
  return len;
}

} // namespace

AssemblyPlan planAssembly(const std::vector<ShotCandidate>& pool,
                         const MusicGrid& music,
                         const PacingTarget& pace,
                         double targetSec,
                         quint32 seed) {
  AssemblyPlan plan;
  const double T = targetSec > 0 ? targetSec : music.durationSec;
  if (T <= 0) {
    plan.ok = false;
    plan.error = QStringLiteral("empty shot pool");
    return plan;
  }

  // 1. Filter candidates
  std::vector<ShotCandidate> valid;
  for (const auto& cand : pool) {
    if (cand.black || cand.freeze) continue;
    if ((cand.endSec - cand.startSec) < pace.minShot - 1e-6) continue;
    valid.push_back(cand);
  }

  if (valid.empty()) {
    plan.ok = false;
    plan.error = QStringLiteral("empty shot pool");
    return plan;
  }

  // 2. Score candidates
  double minSharp = valid[0].sharpness, maxSharp = valid[0].sharpness;
  double minMotion = valid[0].motion, maxMotion = valid[0].motion;
  for (const auto& c : valid) {
    minSharp = std::min(minSharp, c.sharpness);
    maxSharp = std::max(maxSharp, c.sharpness);
    minMotion = std::min(minMotion, c.motion);
    maxMotion = std::max(maxMotion, c.motion);
  }

  std::vector<double> baseScores(valid.size());
  for (size_t i = 0; i < valid.size(); ++i) {
    const auto& c = valid[i];
    const double normSharp = normVal(c.sharpness, minSharp, maxSharp);
    const double normMotion = normVal(c.motion, minMotion, maxMotion);
    const double lumaFactor = 1.0 - std::abs(c.lumaAvg - 128.0) / 128.0;
    const double clipFactor = 1.0 - std::min(c.clippedPct, 50.0) / 50.0;
    baseScores[i] = 0.45 * normSharp + 0.25 * lumaFactor + 0.2 * clipFactor + 0.1 * normMotion;
  }

  // 3. Walk timeline from t = 0 to T
  double t = 0.0;
  bool isFirst = true;
  int slotIndex = 0;
  QString prevAssetId;
  std::map<QString, int> assetUses;
  std::vector<double> candOffsets(valid.size(), 0.0);
  std::vector<PlacedShot> shots;

  while (t < T - 1e-6) {
    const double len = sectionTargetLen(t, isFirst, music, pace);
    const double targetCut = t + len;

    // Prefer a downbeat if one lies within +-25% of len
    double bestDownbeat = -1.0;
    double bestDownbeatDist = 1e9;
    for (double d : music.downbeats) {
      const double dur = d - t;
      if (dur >= pace.minShot - 1e-6 && dur <= pace.maxShot + 1e-6) {
        if (std::abs(dur - len) <= 0.25 * len + 1e-6) {
          const double dist = std::abs(d - targetCut);
          if (dist < bestDownbeatDist) {
            bestDownbeatDist = dist;
            bestDownbeat = d;
          }
        }
      }
    }

    double c = -1.0;
    if (bestDownbeat >= 0.0) {
      c = bestDownbeat;
    } else {
      double bestBeat = -1.0;
      double bestBeatDist = 1e9;
      for (double b : music.beats) {
        const double dur = b - t;
        if (dur >= pace.minShot - 1e-6 && dur <= pace.maxShot + 1e-6) {
          const double dist = std::abs(b - targetCut);
          if (dist < bestBeatDist) {
            bestBeatDist = dist;
            bestBeat = b;
          }
        }
      }
      if (bestBeat >= 0.0) {
        c = bestBeat;
      } else {
        c = targetCut;
      }
    }

    // The last shot ends exactly at T
    if (c >= T - 1e-6 || (T - c) < pace.minShot - 1e-6) {
      c = T;
    }

    double slotLen = c - t;

    // 4. Pick candidate for slot
    std::vector<int> candidateIndices;
    for (int i = 0; i < static_cast<int>(valid.size()); ++i) {
      if ((valid[i].endSec - valid[i].startSec) >= slotLen - 1e-6) {
        if (valid[i].assetId != prevAssetId) {
          candidateIndices.push_back(i);
        }
      }
    }
    if (candidateIndices.empty()) {
      for (int i = 0; i < static_cast<int>(valid.size()); ++i) {
        if ((valid[i].endSec - valid[i].startSec) >= slotLen - 1e-6) {
          candidateIndices.push_back(i);
        }
      }
    }
    // If none fits, use the longest one and shorten the slot to the candidate's length, then report in warnings.
    if (candidateIndices.empty()) {
      double maxLen = 0.0;
      for (const auto& cand : valid) {
        maxLen = std::max(maxLen, cand.endSec - cand.startSec);
      }
      for (int i = 0; i < static_cast<int>(valid.size()); ++i) {
        if ((valid[i].endSec - valid[i].startSec) >= maxLen - 1e-6) {
          candidateIndices.push_back(i);
        }
      }
      const double origSlotLen = slotLen;
      slotLen = maxLen;
      c = t + slotLen;
      plan.warnings.append(QStringLiteral("Candidate shorter than slot: shortened slot from %1 s to %2 s")
                               .arg(origSlotLen, 0, 'f', 2).arg(slotLen, 0, 'f', 2));
    }

    int chosenIndex = -1;
    double bestScore = -1e9;
    for (int idx : candidateIndices) {
      const auto& cand = valid[idx];
      const int reuseCount = assetUses[cand.assetId];
      const double score = baseScores[idx] - 0.35 * reuseCount;
      if (chosenIndex == -1) {
        chosenIndex = idx;
        bestScore = score;
      } else if (score > bestScore + 1e-6) {
        chosenIndex = idx;
        bestScore = score;
      } else if (std::abs(score - bestScore) <= 1e-6) {
        const size_t hashA = qHash(cand.assetId + QString::number(slotIndex), seed);
        const size_t hashB = qHash(valid[chosenIndex].assetId + QString::number(slotIndex), seed);
        if (hashA > hashB) {
          chosenIndex = idx;
          bestScore = score;
        } else if (hashA == hashB && cand.assetId < valid[chosenIndex].assetId) {
          chosenIndex = idx;
          bestScore = score;
        }
      }
    }

    const auto& cand = valid[chosenIndex];
    assetUses[cand.assetId]++;
    prevAssetId = cand.assetId;

    const double curOffset = candOffsets[chosenIndex];
    double srcIn = cand.startSec + curOffset;
    double srcOut = srcIn + slotLen;
    if (srcOut > cand.endSec + 1e-6) {
      srcIn = cand.startSec;
      srcOut = srcIn + slotLen;
      candOffsets[chosenIndex] = slotLen;
    } else {
      candOffsets[chosenIndex] += slotLen;
    }

    // Never repeat the identical source range back to back. Use a different in-point.
    // If none exists, report insufficient_footage in warnings.
    if (!shots.empty() && shots.back().assetId == cand.assetId &&
        std::abs(shots.back().srcInSec - srcIn) < 1e-4 &&
        std::abs(shots.back().srcOutSec - srcOut) < 1e-4) {
      const double candLen = cand.endSec - cand.startSec;
      if (candLen - slotLen > 1e-4) {
        if (std::abs(srcIn - cand.startSec) < 1e-4) {
          srcIn = cand.endSec - slotLen;
        } else {
          srcIn = cand.startSec;
        }
        srcOut = srcIn + slotLen;
        candOffsets[chosenIndex] = (srcIn - cand.startSec) + slotLen;
      } else {
        plan.warnings.append(QStringLiteral("insufficient_footage"));
      }
    }

    PlacedShot ps;
    ps.assetId = cand.assetId;
    ps.srcInSec = srcIn;
    ps.srcOutSec = srcOut;
    ps.timelineStartSec = t;
    ps.timelineEndSec = c;
    ps.reason = QStringLiteral("score %1, len %2s").arg(bestScore, 0, 'f', 2).arg(slotLen, 0, 'f', 2);
    shots.push_back(ps);

    t = c;
    isFirst = false;
    slotIndex++;
  }

  // 5. Fill plan
  plan.ok = true;
  plan.shots = shots;
  plan.totalSec = T;
  plan.cuts = static_cast<int>(shots.size()) - 1;
  plan.cutsOnBeat = 0;

  for (size_t i = 0; i + 1 < shots.size(); ++i) {
    const double cutTime = shots[i].timelineEndSec;
    bool onBeat = false;
    for (double b : music.beats) {
      if (std::abs(cutTime - b) <= 0.001) {
        onBeat = true;
        break;
      }
    }
    if (onBeat) {
      plan.cutsOnBeat++;
    }
  }

  plan.aslSec = shots.empty() ? 0.0 : (T / static_cast<double>(shots.size()));

  // Notes: one line per section
  for (const auto& sec : music.sections) {
    if (sec.start >= T) continue;
    int count = 0;
    for (const auto& s : shots) {
      if (s.timelineStartSec >= sec.start - 1e-6 && s.timelineStartSec < sec.end - 1e-6) {
        count++;
      }
    }
    if (count > 0) {
      const double secDur = std::min(sec.end, T) - std::min(sec.start, T);
      const double secAsl = secDur / count;
      plan.notes.append(QStringLiteral("%1 %2-%3 s: %4 shots, ASL %5 s")
                            .arg(sec.label.isEmpty() ? QStringLiteral("section") : sec.label)
                            .arg(sec.start, 0, 'f', 1)
                            .arg(sec.end, 0, 'f', 1)
                            .arg(count)
                            .arg(secAsl, 0, 'f', 2));
    }
  }

  const double cutsPct = (plan.cuts > 0) ? (100.0 * plan.cutsOnBeat / plan.cuts) : 100.0;
  if (cutsPct < pace.cutsOnBeatMinPct) {
    plan.notes.append(QStringLiteral("Warning: cuts on beat %1% is below target %2%")
                          .arg(cutsPct, 0, 'f', 1)
                          .arg(pace.cutsOnBeatMinPct));
  }
  if (plan.aslSec < pace.aslMin || plan.aslSec > pace.aslMax) {
    plan.notes.append(QStringLiteral("Warning: ASL %1 s is outside target [%2, %3] s")
                          .arg(plan.aslSec, 0, 'f', 2)
                          .arg(pace.aslMin, 0, 'f', 2)
                          .arg(pace.aslMax, 0, 'f', 2));
  }

  return plan;
}

} // namespace sf::engine
