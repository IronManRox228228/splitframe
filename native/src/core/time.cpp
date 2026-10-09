#include "core/time.h"

#include <algorithm>
#include <cmath>

namespace sf {

// Math.round semantics (half toward +infinity), so frame numbers match the TS app exactly. Not
// floor(v + 0.5): that rounds 0.49999999999999994 up.
std::int64_t jsRound(double v) {
  const double f = std::floor(v);
  return static_cast<std::int64_t>(v - f >= 0.5 ? f + 1.0 : f);
}

Frame secondsToFrames(double seconds, double fps) { return jsRound(seconds * fps); }

double framesToSeconds(Frame frames, double fps) { return static_cast<double>(frames) / fps; }

Ms framesToMs(Frame frames, double fps) { return jsRound(static_cast<double>(frames) / fps * 1000.0); }

Frame msToFrames(Ms ms, double fps) { return jsRound(static_cast<double>(ms) / 1000.0 * fps); }

QString formatTimecode(Frame frames, double fps) {
  const std::int64_t tenths = jsRound(static_cast<double>(std::max<Frame>(0, frames)) / fps * 10.0);
  const std::int64_t t = tenths % 10;
  const std::int64_t total = tenths / 10;
  const std::int64_t s = total % 60;
  const std::int64_t m = (total / 60) % 60;
  const std::int64_t h = total / 3600;
  const auto pad = [](std::int64_t n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); };
  if (h > 0) return QStringLiteral("%1:%2:%3.%4").arg(pad(h), pad(m), pad(s)).arg(t);
  return QStringLiteral("%1:%2.%3").arg(pad(m), pad(s)).arg(t);
}

} // namespace sf
