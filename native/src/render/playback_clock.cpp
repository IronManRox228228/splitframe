#include "render/playback_clock.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sf::render {

std::int64_t SteadyClockSource::nowNs() const {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

PlaybackClock::PlaybackClock(std::shared_ptr<ClockSource> source) : source_(std::move(source)) {}

void PlaybackClock::setTimeline(double fps, Frame durationFrames) {
  const std::lock_guard lock(m_);
  fps_ = fps > 0 ? fps : 30;
  duration_ = std::max<Frame>(0, durationFrames);
}

void PlaybackClock::setLoop(bool loop) {
  const std::lock_guard lock(m_);
  loop_ = loop;
}

double PlaybackClock::positionLocked(std::int64_t nowNs) const {
  if (!playing_) return anchorFrame_;
  const double seconds = static_cast<double>(nowNs - anchorNs_) * 1e-9;
  return anchorFrame_ + seconds * fps_ * rate_;
}

void PlaybackClock::anchorLocked(Frame frame, std::int64_t nowNs) {
  anchorFrame_ = static_cast<double>(frame);
  anchorNs_ = nowNs;
}

Frame PlaybackClock::clampFrame(double pos) const {
  // the frame whose display interval has begun; the epsilon (about 30 ns) keeps t = N/fps on frame N despite integer-nanosecond rounding
  const auto f = static_cast<Frame>(std::floor(pos + 1e-6));
  return std::clamp<Frame>(f, 0, std::max<Frame>(0, duration_ - 1));
}

void PlaybackClock::seek(Frame frame) {
  const std::lock_guard lock(m_);
  anchorLocked(std::clamp<Frame>(frame, 0, std::max<Frame>(0, duration_ - 1)), source_->nowNs());
}

void PlaybackClock::play(double rate) {
  const std::lock_guard lock(m_);
  const std::int64_t now = source_->nowNs();
  const Frame here = clampFrame(positionLocked(now));
  playing_ = rate != 0;
  rate_ = rate != 0 ? rate : rate_;
  // starting forward on the last frame (or backward on the first) wraps to the other end
  Frame start = here;
  if (playing_ && duration_ > 0) {
    if (rate_ > 0 && here >= duration_ - 1) start = 0;
    if (rate_ < 0 && here <= 0) start = duration_ - 1;
  }
  anchorLocked(start, now);
}

void PlaybackClock::pause() {
  const std::lock_guard lock(m_);
  if (!playing_) return;
  const std::int64_t now = source_->nowNs();
  const Frame here = clampFrame(positionLocked(now));
  playing_ = false;
  anchorLocked(here, now);
}

void PlaybackClock::setRate(double rate) {
  const std::lock_guard lock(m_);
  if (rate == 0) return;
  const std::int64_t now = source_->nowNs();
  const double pos = positionLocked(now);
  rate_ = rate;
  anchorFrame_ = pos; // keep the fractional part so a rate change doesn't jitter
  anchorNs_ = now;
}

PlaybackClock::Position PlaybackClock::poll() { return poll(source_->nowNs()); }

PlaybackClock::Position PlaybackClock::poll(std::int64_t nowNs) {
  const std::lock_guard lock(m_);
  Position out;
  out.rate = rate_;
  double pos = positionLocked(nowNs);
  if (playing_ && duration_ > 0) {
    const double last = static_cast<double>(duration_ - 1);
    if (loop_ && duration_ > 1) {
      if (pos >= static_cast<double>(duration_) || pos < 0) {
        pos = std::fmod(pos, static_cast<double>(duration_));
        if (pos < 0) pos += static_cast<double>(duration_);
        anchorFrame_ = pos; // re-anchor so the arithmetic stays small on long loops
        anchorNs_ = nowNs;
      }
    } else if ((rate_ > 0 && pos >= last) || (rate_ < 0 && pos <= 0)) {
      playing_ = false;
      anchorLocked(rate_ > 0 ? duration_ - 1 : 0, nowNs);
      pos = anchorFrame_;
      out.reachedEnd = true;
    }
  }
  out.frame = clampFrame(pos);
  out.playing = playing_;
  return out;
}

double PlaybackClock::shuttleRate(double currentRate, bool playing, int dir) {
  const double first = dir < 0 ? -1.0 : 1.0;
  if (!playing || (currentRate > 0) != (dir > 0)) return first;
  return std::abs(currentRate) >= 8 ? currentRate : currentRate * 2;
}

Frame PlaybackClock::frame() const {
  const std::lock_guard lock(m_);
  return clampFrame(positionLocked(source_->nowNs()));
}

bool PlaybackClock::playing() const {
  const std::lock_guard lock(m_);
  return playing_;
}

double PlaybackClock::rate() const {
  const std::lock_guard lock(m_);
  return rate_;
}

double PlaybackClock::fps() const {
  const std::lock_guard lock(m_);
  return fps_;
}

Frame PlaybackClock::duration() const {
  const std::lock_guard lock(m_);
  return duration_;
}

void PacingStats::present(Frame frame, bool playing, double rate) {
  ++presented_;
  if (playing && last_ >= 0) {
    const Frame step = rate >= 0 ? frame - last_ : last_ - frame;
    if (step > 1) dropped_ += step - 1;
  }
  last_ = frame;
}

void PacingStats::reset() {
  last_ = -1;
  presented_ = dropped_ = 0;
}

} // namespace sf::render
