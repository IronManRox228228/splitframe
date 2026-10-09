#include "media/thumbnails.h"

#include "media/ffmpeg.h"
#include "media/frame_convert.h"
#include "media/packet_index.h"

#include <QTransform>

#include <algorithm>
#include <cmath>

namespace sf {

std::vector<Thumbnail> extractThumbnailStrip(const QString& path, int count, QSize maxSize, const std::function<bool()>& cancel,
                                             QString* error) {
  std::vector<Thumbnail> out;
  auto fail = [&](const QString& msg) {
    if (error) *error = msg;
    return std::vector<Thumbnail>{};
  };
  if (count <= 0) return out;
  QString err;
  av::FormatPtr fmt = av::openInput(path, &err);
  if (!fmt) return fail(err);
  const AVCodec* codec = nullptr;
  const int v = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (v < 0 || !codec) return fail(QStringLiteral("%1 has no decodable video stream").arg(path));
  AVStream* st = fmt->streams[v];
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (static_cast<int>(i) != v) fmt->streams[i]->discard = AVDISCARD_ALL;
  }
  av::CodecPtr ctx(avcodec_alloc_context3(codec));
  if (!ctx || avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0) return fail(QStringLiteral("Can't set up decoder"));
  ctx->pkt_timebase = st->time_base;
  ctx->thread_count = 1;            // one frame per seek: threads only add pipeline latency
  ctx->skip_frame = AVDISCARD_NONKEY;
  if (const int rc = avcodec_open2(ctx.get(), codec, nullptr); rc < 0) return fail(QStringLiteral("Can't open decoder: %1").arg(av::errorString(rc)));

  const int rotation = media::displayRotation(st->codecpar);
  const bool swap = rotation == 90 || rotation == 270;
  const QSize coded(st->codecpar->width, st->codecpar->height);
  QSize shown = swap ? coded.transposed() : coded;
  shown.scale(maxSize.isEmpty() ? QSize(160, 90) : maxSize, Qt::KeepAspectRatio);
  shown = QSize(std::max(2, shown.width() & ~1), std::max(2, shown.height() & ~1));
  const QSize stored = swap ? shown.transposed() : shown;

  const double base = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / double(AV_TIME_BASE) : 0.0;
  double duration = fmt->duration > 0 ? fmt->duration / double(AV_TIME_BASE) : 0.0;
  if (duration <= 0 && st->duration > 0) duration = st->duration * av_q2d(st->time_base);
  const double tb = av_q2d(st->time_base);

  media::FrameConverter converter;
  av::PacketPtr pkt(av_packet_alloc());
  av::FramePtr frame(av_frame_alloc());
  for (int i = 0; i < count; ++i) {
    if (cancel && cancel()) return {};
    Thumbnail t;
    t.requestedSec = duration * (i + 0.5) / count;
    const int64_t ts = static_cast<int64_t>(std::llround((t.requestedSec + base) / tb));
    if (av_seek_frame(fmt.get(), v, ts, AVSEEK_FLAG_BACKWARD) < 0 && av_seek_frame(fmt.get(), -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
      return fail(QStringLiteral("Seek failed in %1").arg(path));
    }
    avcodec_flush_buffers(ctx.get());

    bool got = false;
    bool draining = false;
    while (!got) {
      const int rc = avcodec_receive_frame(ctx.get(), frame.get());
      if (rc == 0) {
        t.actualSec = frame->pts != AV_NOPTS_VALUE ? frame->pts * tb - base : t.requestedSec;
        if (!out.empty() && std::abs(out.back().actualSec - t.actualSec) < 1e-6) {
          t.image = out.back().image; // same keyframe as the previous slot: share the pixels
        } else {
          t.image = converter.toRgba(frame.get(), stored);
          if (rotation != 0) t.image = t.image.transformed(QTransform().rotate(rotation), Qt::SmoothTransformation);
        }
        av_frame_unref(frame.get());
        got = true;
      } else if (rc == AVERROR(EAGAIN)) {
        if (draining) break;
        if (av_read_frame(fmt.get(), pkt.get()) < 0) {
          draining = true;
          avcodec_send_packet(ctx.get(), nullptr);
        } else {
          if (pkt->stream_index == v) avcodec_send_packet(ctx.get(), pkt.get());
          av_packet_unref(pkt.get());
        }
      } else if (rc == AVERROR_INVALIDDATA) {
        continue;
      } else {
        break;
      }
    }
    if (!got || t.image.isNull()) {
      if (out.empty()) return fail(QStringLiteral("No keyframe found near %1 s in %2").arg(t.requestedSec).arg(path));
      t.actualSec = out.back().actualSec; // an unreadable slot reuses its neighbour rather than leaving a hole
      t.image = out.back().image;
    }
    out.push_back(std::move(t));
  }
  return out;
}

} // namespace sf
