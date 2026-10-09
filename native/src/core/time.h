#pragma once

#include <QString>
#include <cstdint>

// Timeline time is integer frames at the project fps; analysis timestamps (transcripts, scenes,
// beat maps) are integer milliseconds. Same model as packages/schema/src/time.ts, so project
// documents written by the TypeScript app load unchanged.
namespace sf {

using Frame = std::int64_t;
using Ms = std::int64_t;

Frame secondsToFrames(double seconds, double fps);
double framesToSeconds(Frame frames, double fps);
Ms framesToMs(Frame frames, double fps);
Frame msToFrames(Ms ms, double fps);

// "00:07.2" / "01:02:03.0"; rounds to tenths first so 59.96 s prints 01:00.0, not 00:60.0
QString formatTimecode(Frame frames, double fps);

} // namespace sf
