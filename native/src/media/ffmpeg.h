#pragma once

// FFmpeg is C: include it in one place, and give its objects owners so nothing leaks on an early return.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QString>
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
  void operator()(SwsContext* s) const { sws_free_context(&s); }
};
struct SwrFree {
  void operator()(SwrContext* s) const { swr_free(&s); }
};
struct BufferUnref {
  void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFree>;
using SwrPtr = std::unique_ptr<SwrContext, SwrFree>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferUnref>;

// "Invalid data found when processing input" instead of a bare negative number.
inline QString errorString(int code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buf, sizeof buf);
  return QString::fromUtf8(buf);
}

// Opens a container and reads stream headers. nullptr + *error on failure. Paths go in as UTF-8.
FormatPtr openInput(const QString& path, QString* error);

} // namespace sf::av
