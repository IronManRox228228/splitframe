#include "media/probe.h"

#include "media/ffmpeg.h"

namespace sf {

namespace {

QString avError(int code) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buf, sizeof buf);
  return QString::fromUtf8(buf);
}

void setError(QString* out, const QString& msg) {
  if (out) *out = msg;
}

av::FormatPtr openInput(const QString& path, QString* error) {
  AVFormatContext* raw = nullptr;
  const QByteArray utf8 = path.toUtf8();
  if (const int rc = avformat_open_input(&raw, utf8.constData(), nullptr, nullptr); rc < 0) {
    setError(error, QStringLiteral("Can't open %1: %2").arg(path, avError(rc)));
    return nullptr;
  }
  av::FormatPtr fmt(raw);
  if (const int rc = avformat_find_stream_info(fmt.get(), nullptr); rc < 0) {
    setError(error, QStringLiteral("Can't read streams of %1: %2").arg(path, avError(rc)));
    return nullptr;
  }
  return fmt;
}

} // namespace

QString ffmpegVersion() { return QString::fromUtf8(av_version_info()); }

std::optional<MediaInfo> probeMedia(const QString& path, QString* error) {
  const av::FormatPtr fmt = openInput(path, error);
  if (!fmt) return std::nullopt;
  MediaInfo info;
  if (fmt->duration > 0) info.durationMs = av_rescale(fmt->duration, 1000, AV_TIME_BASE);
  const int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (v >= 0) {
    const AVStream* s = fmt->streams[v];
    info.hasVideo = true;
    info.width = s->codecpar->width;
    info.height = s->codecpar->height;
    const AVRational rate = av_guess_frame_rate(fmt.get(), const_cast<AVStream*>(s), nullptr);
    if (rate.num > 0 && rate.den > 0) info.fps = av_q2d(rate);
    info.videoCodec = QString::fromUtf8(avcodec_get_name(s->codecpar->codec_id));
  }
  info.hasAudio = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
  return info;
}

QImage decodeFrame(const QString& path, qint64 atMs, QString* error) {
  const av::FormatPtr fmt = openInput(path, error);
  if (!fmt) return {};
  const AVCodec* codec = nullptr;
  const int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (v < 0 || !codec) {
    setError(error, QStringLiteral("%1 has no decodable video stream").arg(path));
    return {};
  }
  AVStream* stream = fmt->streams[v];
  av::CodecPtr dec(avcodec_alloc_context3(codec));
  avcodec_parameters_to_context(dec.get(), stream->codecpar);
  if (const int rc = avcodec_open2(dec.get(), codec, nullptr); rc < 0) {
    setError(error, QStringLiteral("Can't open decoder: %1").arg(avError(rc)));
    return {};
  }

  const int64_t target = av_rescale_q(atMs, AVRational{1, 1000}, stream->time_base);
  if (atMs > 0) av_seek_frame(fmt.get(), v, target, AVSEEK_FLAG_BACKWARD);

  av::PacketPtr pkt(av_packet_alloc());
  av::FramePtr frame(av_frame_alloc());
  bool flushing = false;
  while (true) {
    if (!flushing) {
      const int rc = av_read_frame(fmt.get(), pkt.get());
      if (rc < 0) {
        flushing = true;
        avcodec_send_packet(dec.get(), nullptr);
      } else {
        if (pkt->stream_index == v) avcodec_send_packet(dec.get(), pkt.get());
        av_packet_unref(pkt.get());
      }
    }
    const int rc = avcodec_receive_frame(dec.get(), frame.get());
    if (rc == AVERROR(EAGAIN) && !flushing) continue;
    if (rc < 0) break;
    // after a seek, decode forward to the frame that covers the requested time
    if (atMs > 0 && frame->best_effort_timestamp != AV_NOPTS_VALUE && frame->best_effort_timestamp < target &&
        frame->duration > 0 && frame->best_effort_timestamp + frame->duration <= target) {
      av_frame_unref(frame.get());
      continue;
    }
    QImage out(frame->width, frame->height, QImage::Format_RGBA8888);
    av::SwsPtr sws(sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                                  frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!sws) {
      setError(error, QStringLiteral("Unsupported pixel format"));
      return {};
    }
    uint8_t* dst[1] = {out.bits()};
    const int dstStride[1] = {static_cast<int>(out.bytesPerLine())};
    sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    return out;
  }
  setError(error, QStringLiteral("No frame at %1 ms in %2").arg(atMs).arg(path));
  return {};
}

} // namespace sf
