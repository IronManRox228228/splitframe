#pragma once

// FFmpeg is C: include it in one place, and give its objects owners so nothing leaks on an early return.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <memory>

namespace sf::av {

struct FormatCloser {
  void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecCloser {
  void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameFree {
  void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketFree {
  void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct SwsFree {
  void operator()(SwsContext* s) const { sws_freeContext(s); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFree>;

} // namespace sf::av
