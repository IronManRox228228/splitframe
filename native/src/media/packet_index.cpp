#include "media/packet_index.h"

#include <algorithm>
#include <cmath>

namespace sf::media {

namespace {

struct RawPacket {
  int64_t pts;
  bool key;
  int64_t pos;
};

// MPEG program/transport streams and raw elementary streams often carry no PTS on P/B pictures.
// Without timestamps the only truth is order, so lay frames on a constant-rate grid anchored at the
// earliest known timestamp, and pin keyframes to their grid slots (I-frames always have a PTS).
PacketScan gridFromOrder(const std::vector<RawPacket>& raw, const AVStream* st, AVRational rate) {
  PacketScan out;
  out.synthesized = true;
  int64_t t0 = AV_NOPTS_VALUE;
  for (const RawPacket& p : raw) {
    if (p.pts != AV_NOPTS_VALUE && (t0 == AV_NOPTS_VALUE || p.pts < t0)) t0 = p.pts;
  }
  if (t0 == AV_NOPTS_VALUE) t0 = 0;
  const double fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 25.0;
  const double d = 1.0 / (fps * av_q2d(st->time_base)); // ticks per frame
  out.frames.resize(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) out.frames[i] = {t0 + std::llround(static_cast<double>(i) * d), false, -1};
  for (const RawPacket& p : raw) {
    if (!p.key || p.pts == AV_NOPTS_VALUE) continue;
    const auto slot = std::clamp<int64_t>(std::llround((p.pts - t0) / d), 0, static_cast<int64_t>(raw.size()) - 1);
    out.frames[static_cast<size_t>(slot)].key = true;
    out.frames[static_cast<size_t>(slot)].pos = p.pos;
  }
  return out;
}

} // namespace

PacketScan scanVideoPackets(AVFormatContext* fmt, int stream, int64_t maxPackets, const std::function<bool()>& cancel) {
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (static_cast<int>(i) != stream) fmt->streams[i]->discard = AVDISCARD_ALL;
  }
  av::PacketPtr pkt(av_packet_alloc());
  std::vector<RawPacket> raw;
  size_t missing = 0;
  bool eof = false;
  while (true) {
    if (cancel && cancel()) break;
    if (maxPackets > 0 && static_cast<int64_t>(raw.size()) >= maxPackets) break;
    const int rc = av_read_frame(fmt, pkt.get());
    if (rc < 0) {
      eof = rc == AVERROR_EOF;
      break;
    }
    // Packets flagged discard (edit-list trimmed) are skipped because the decoder never outputs them.
    if (pkt->stream_index == stream && !(pkt->flags & AV_PKT_FLAG_DISCARD)) {
      raw.push_back({pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0, pkt->pos});
      if (pkt->pts == AV_NOPTS_VALUE) ++missing;
    }
    av_packet_unref(pkt.get());
  }
  AVStream* st = fmt->streams[stream];
  PacketScan out;
  if (missing > 0 && !raw.empty()) {
    out = gridFromOrder(raw, st, av_guess_frame_rate(fmt, st, nullptr));
  } else {
    out.frames.reserve(raw.size());
    for (const RawPacket& p : raw) out.frames.push_back({p.pts, p.key, p.pos});
    std::stable_sort(out.frames.begin(), out.frames.end(),
                     [](const FrameEntry& a, const FrameEntry& b) { return a.pts < b.pts; });
    // a repeated pts is a broken mux; keep the first so indices stay strictly increasing in time
    out.frames.erase(std::unique(out.frames.begin(), out.frames.end(),
                                 [](const FrameEntry& a, const FrameEntry& b) { return a.pts == b.pts; }),
                     out.frames.end());
  }
  out.complete = eof;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) fmt->streams[i]->discard = AVDISCARD_DEFAULT;
  return out;
}

int displayRotation(const AVCodecParameters* par) {
  const AVPacketSideData* sd =
      av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
  if (!sd || sd->size < 9 * sizeof(int32_t)) return 0;
  // FFmpeg reports the counter-clockwise angle to apply; players want clockwise.
  const double ccw = av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
  if (std::isnan(ccw)) return 0;
  const int cw = static_cast<int>(std::lround(-ccw));
  return ((cw % 360) + 360) % 360;
}

int64_t frameIndexForPts(const std::vector<FrameEntry>& frames, int64_t pts) {
  const auto it = std::upper_bound(frames.begin(), frames.end(), pts,
                                   [](int64_t v, const FrameEntry& f) { return v < f.pts; });
  return static_cast<int64_t>(it - frames.begin()) - 1;
}

} // namespace sf::media
