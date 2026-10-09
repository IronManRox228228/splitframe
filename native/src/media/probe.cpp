#include "media/probe.h"

#include "media/ffmpeg.h"
#include "media/packet_index.h"

#include <algorithm>
#include <cmath>

namespace sf {

namespace {

void setError(QString* out, const QString& msg) {
  if (out) *out = msg;
}

QString nameOr(const char* name) { return name ? QString::fromUtf8(name) : QStringLiteral("unspecified"); }

QString rangeName(AVColorRange r) {
  switch (r) {
  case AVCOL_RANGE_MPEG: return QStringLiteral("tv");
  case AVCOL_RANGE_JPEG: return QStringLiteral("pc");
  default: return QStringLiteral("unspecified");
  }
}

// Seconds from the container start to a stream timestamp. 0 when either side is unknown.
double offsetSec(const AVFormatContext* fmt, const AVStream* s, int64_t pts) {
  if (pts == AV_NOPTS_VALUE) return 0;
  const double abs = pts * av_q2d(s->time_base);
  const double base = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / double(AV_TIME_BASE) : 0.0;
  return abs - base;
}


void measureTiming(AVFormatContext* fmt, int stream, const AVStream* s, int limit, MediaInfo& info) {
  const media::PacketScan scan = media::scanVideoPackets(fmt, stream, limit);
  const auto& f = scan.frames;
  info.timingComplete = scan.complete;
  if (scan.complete) info.frameCount = static_cast<qint64>(f.size());
  else if (s->nb_frames > 0) info.frameCount = s->nb_frames;
  if (f.size() < 3) return;

  std::vector<int64_t> gaps;
  gaps.reserve(f.size() - 1);
  for (size_t i = 1; i < f.size(); ++i) gaps.push_back(f[i].pts - f[i - 1].pts);
  std::vector<int64_t> sorted = gaps;
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
  const int64_t median = sorted[sorted.size() / 2];
  const auto [mn, mx] = std::minmax_element(gaps.begin(), gaps.end());

  const double tb = av_q2d(s->time_base);
  info.minFps = *mx > 0 ? 1.0 / (*mx * tb) : 0;
  info.maxFps = *mn > 0 ? 1.0 / (*mn * tb) : 0;
  const int64_t span = f.back().pts - f.front().pts;
  if (span > 0) info.avgFps = (static_cast<double>(f.size() - 1)) / (span * tb);
  // Containers round each timestamp to their time base (mkv: milliseconds), so a 30 fps file
  // legitimately alternates 33/34 ms gaps. Allow 2 ticks or 10 % before calling it variable.
  const int64_t tolerance = std::max<int64_t>(2, median / 10);
  info.vfr = std::any_of(gaps.begin(), gaps.end(), [&](int64_t g) { return std::llabs(g - median) > tolerance; });
}

} // namespace

QString ffmpegVersion() { return QString::fromUtf8(av_version_info()); }

std::optional<MediaInfo> probeMedia(const QString& path, QString* error, const ProbeOptions& options) {
  av::FormatPtr fmt = av::openInput(path, error);
  if (!fmt) return std::nullopt;
  MediaInfo info;
  if (fmt->duration > 0) info.durationMs = av_rescale(fmt->duration, 1000, AV_TIME_BASE);
  const int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (v >= 0) {
    AVStream* s = fmt->streams[v];
    const AVCodecParameters* par = s->codecpar;
    info.hasVideo = true;
    info.width = par->width;
    info.height = par->height;
    const AVRational rate = av_guess_frame_rate(fmt.get(), s, nullptr);
    if (rate.num > 0 && rate.den > 0) info.fps = av_q2d(rate);
    info.videoCodec = QString::fromUtf8(avcodec_get_name(par->codec_id));

    info.rotation = media::displayRotation(par);
    const bool swap = info.rotation == 90 || info.rotation == 270;
    info.displayWidth = swap ? info.height : info.width;
    info.displayHeight = swap ? info.width : info.height;

    const auto pix = static_cast<AVPixelFormat>(par->format);
    if (const char* n = av_get_pix_fmt_name(pix)) info.pixelFormat = QString::fromUtf8(n);
    if (const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pix)) info.bitDepth = d->comp[0].depth;
    info.color.primaries = par->color_primaries == AVCOL_PRI_UNSPECIFIED ? QStringLiteral("unspecified") : nameOr(av_color_primaries_name(par->color_primaries));
    info.color.transfer = par->color_trc == AVCOL_TRC_UNSPECIFIED ? QStringLiteral("unspecified") : nameOr(av_color_transfer_name(par->color_trc));
    info.color.matrix = par->color_space == AVCOL_SPC_UNSPECIFIED ? QStringLiteral("unspecified") : nameOr(av_color_space_name(par->color_space));
    info.color.range = rangeName(par->color_range);

    info.videoStartSec = offsetSec(fmt.get(), s, s->start_time);
    measureTiming(fmt.get(), v, s, options.vfrScanLimit, info);
  }
  const int a = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (a >= 0) {
    const AVStream* s = fmt->streams[a];
    info.hasAudio = true;
    info.audioSampleRate = s->codecpar->sample_rate;
    info.audioChannels = s->codecpar->ch_layout.nb_channels;
    info.audioCodec = QString::fromUtf8(avcodec_get_name(s->codecpar->codec_id));
    info.audioStartSec = offsetSec(fmt.get(), s, s->start_time);
  }
  return info;
}

QImage decodeFrame(const QString& path, qint64 atMs, QString* error) {
  const av::FormatPtr fmt = av::openInput(path, error);
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
    setError(error, QStringLiteral("Can't open decoder: %1").arg(av::errorString(rc)));
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
