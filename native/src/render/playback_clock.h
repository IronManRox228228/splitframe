#pragma once

// Where the playhead is. The position is a pure function of a monotonic time source, an anchor
// (frame + time of the last seek/play/pause) and the rate, so a late frame never shifts later ones:
// whoever presents asks "which frame is it now" and skips whatever it missed instead of drifting.
//
// The time source is pluggable on purpose. Today it is the system's steady clock; when audio
// arrives (M4) a ClockSource reporting the sound card's played-sample position slots in as the
// master and video follows it, with no change here.

#include "core/time.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace sf::render {

class ClockSource {
public:
  virtual ~ClockSource() = default;
  virtual std::int64_t nowNs() const = 0; // monotonic
};

class SteadyClockSource final : public ClockSource {
public:
  std::int64_t nowNs() const override;
};

// Thread-safe: the render thread polls it while the UI thread seeks and plays.
class PlaybackClock {
public:
  explicit PlaybackClock(std::shared_ptr<ClockSource> source = std::make_shared<SteadyClockSource>());

  // Frames [0, durationFrames) exist. Playing past the end stops on the last frame (or wraps, with loop).
  void setTimeline(double fps, Frame durationFrames);
  void setLoop(bool loop);

  void seek(Frame frame); // keeps playing if it was
  // rate 1 = real time, 2 = double speed, negative = backwards. Starts from the current frame.
  void play(double rate = 1.0);
  void pause();
  void setRate(double rate); // while playing: continue from the current frame at the new rate

  struct Position {
    Frame frame = 0;
    bool playing = false;
    double rate = 0;
    bool reachedEnd = false; // this poll is the one that stopped playback at an end
  };
  // The frame to show now. Applies end-of-timeline handling, so call it from the one place that
  // presents; frame()/playing() are the side-effect-free peeks.
  Position poll();
  Position poll(std::int64_t nowNs);
  // J / K / L: the rate a press of `dir` (-1 = J, +1 = L) leads to. The first press plays at 1x in that
  // direction, repeats in the same direction double it up to 8x, the opposite direction restarts at 1x.
  static double shuttleRate(double currentRate, bool playing, int dir);

  Frame frame() const;
  bool playing() const;
  double rate() const;
  double fps() const;
  Frame duration() const;

private:
  double positionLocked(std::int64_t nowNs) const; // fractional frames
  void anchorLocked(Frame frame, std::int64_t nowNs);
  Frame clampFrame(double pos) const;

  mutable std::mutex m_;
  std::shared_ptr<ClockSource> source_;
  double fps_ = 30;
  Frame duration_ = 0;
  bool loop_ = false;
  bool playing_ = false;
  double rate_ = 1;
  double anchorFrame_ = 0;
  std::int64_t anchorNs_ = 0;
};

// How well playback keeps up: frames the presenter showed vs frames the clock skipped between them.
class PacingStats {
public:
  // Call with each presented frame in order. A forward step of more than one frame while playing
  // counts the difference as dropped.
  void present(Frame frame, bool playing, double rate);
  void reset();
  std::int64_t presented() const { return presented_; }
  std::int64_t dropped() const { return dropped_; }

private:
  Frame last_ = -1;
  std::int64_t presented_ = 0;
  std::int64_t dropped_ = 0;
};

} // namespace sf::render
