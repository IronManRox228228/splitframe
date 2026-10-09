#include "media/frame_convert.h"

#include <QThread>
#include <algorithm>

namespace sf::media {

FrameConverter::FrameConverter() : sws_(sws_alloc_context()) {
  if (!sws_) return;
  sws_->flags = SWS_BILINEAR;
  sws_->threads = std::clamp(QThread::idealThreadCount() / 2, 1, 4);
}

QImage FrameConverter::toRgba(AVFrame* src, QSize out) {
  if (!sws_ || !src || src->width <= 0 || src->height <= 0) return {};
  const QSize size = out.isEmpty() ? QSize(src->width, src->height) : out;
  // Untagged video is BT.709 from 720p up and BT.601 below, which is what players do. swscale would
  // otherwise assume 601 and turn HD footage slightly green/magenta.
  if (src->colorspace == AVCOL_SPC_UNSPECIFIED) {
    src->colorspace = src->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
  }
  // yuvj* are the deprecated spelling of "full range yuv*"; say so properly instead of getting a warning per frame
  switch (src->format) {
  case AV_PIX_FMT_YUVJ420P: src->format = AV_PIX_FMT_YUV420P; src->color_range = AVCOL_RANGE_JPEG; break;
  case AV_PIX_FMT_YUVJ422P: src->format = AV_PIX_FMT_YUV422P; src->color_range = AVCOL_RANGE_JPEG; break;
  case AV_PIX_FMT_YUVJ444P: src->format = AV_PIX_FMT_YUV444P; src->color_range = AVCOL_RANGE_JPEG; break;
  case AV_PIX_FMT_YUVJ440P: src->format = AV_PIX_FMT_YUV440P; src->color_range = AVCOL_RANGE_JPEG; break;
  default: break;
  }
  QImage image(size, QImage::Format_RGBA8888);
  if (image.isNull()) return {};

  av::FramePtr dst(av_frame_alloc());
  dst->format = AV_PIX_FMT_RGBA;
  dst->width = size.width();
  dst->height = size.height();
  dst->data[0] = image.bits();
  dst->linesize[0] = static_cast<int>(image.bytesPerLine());
  dst->colorspace = AVCOL_SPC_RGB;
  dst->color_range = AVCOL_RANGE_JPEG;
  // same primaries/transfer in and out: we only want the matrix and range handled, not tone mapping
  dst->color_primaries = src->color_primaries;
  dst->color_trc = src->color_trc;
  if (sws_scale_frame(sws_.get(), dst.get(), src) < 0) return {};
  return image;
}

} // namespace sf::media
