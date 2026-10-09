#pragma once

// Internal: a cheap pre-pass over a video stream's packets (no decoding). The result is the frame
// table every other part relies on: frame N is the Nth presented frame, whatever the container's
// time base or frame-rate pattern (VFR included), and the keyframe flags say where decoding can start.

#include "media/ffmpeg.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace sf::media {

struct FrameEntry {
  int64_t pts = 0; // stream time base
  bool key = false;
  int64_t pos = -1; // byte offset of the packet (-1 unknown); lets streams without usable PTS seek exactly
};

struct PacketScan {
  std::vector<FrameEntry> frames; // presentation order, unique pts
  bool complete = false;          // false when maxPackets cut the scan short or cancel fired
  // True when the stream had packets without a PTS (typical MPEG-PS/TS/ES with B-frames). The pts
  // values are then a constant-rate grid, and decoded frames must be numbered by order, not looked up.
  bool synthesized = false;
};

// Reads packets of `stream` until EOF (or maxPackets, 0 = unlimited). Moves the demuxer: seek afterwards.
// Packets flagged discard (edit-list trimmed) are skipped because the decoder never outputs them.
PacketScan scanVideoPackets(AVFormatContext* fmt, int stream, int64_t maxPackets = 0,
                            const std::function<bool()>& cancel = {});

// Degrees clockwise to turn decoded frames upright, from the stream's display matrix (0 when absent).
int displayRotation(const AVCodecParameters* par);

// Index of the frame with exactly this pts, else the last frame before it, else -1.
int64_t frameIndexForPts(const std::vector<FrameEntry>& frames, int64_t pts);

} // namespace sf::media
