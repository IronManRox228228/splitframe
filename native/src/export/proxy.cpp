#include "export/proxy.h"

#include "media/ffmpeg.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <cstring>

namespace sf::xport {

namespace {

QString hashOf(const QString& s, int chars) {
  return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex()).left(chars);
}

QString normalised(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower(); }

QString pathHash(const QString& sourcePath) { return hashOf(normalised(sourcePath), 10); }

QString keyHash(const QString& sourcePath) {
  const QFileInfo fi(sourcePath);
  return hashOf(QStringLiteral("%1|%2|%3").arg(normalised(sourcePath)).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()), 10);
}

int evenInt(double v) { return std::max(2, static_cast<int>(v / 2.0 + 0.5) * 2); }

} // namespace

QString proxyFileName(const QString& sourcePath) { return pathHash(sourcePath) + QLatin1Char('_') + keyHash(sourcePath) + QStringLiteral(".mp4"); }

QString proxyCacheDir(const QString& projectFilePath) {
  if (!projectFilePath.isEmpty()) {
    const QFileInfo fi(projectFilePath);
    return QDir(fi.absolutePath()).absoluteFilePath(fi.completeBaseName() + QStringLiteral(".proxies"));
  }
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).absoluteFilePath(QStringLiteral("proxies"));
}

QString proxyPathFor(const QString& sourcePath, const QString& cacheDir) { return QDir(cacheDir).absoluteFilePath(proxyFileName(sourcePath)); }

QString existingProxy(const QString& sourcePath, const QString& cacheDir) {
  if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) return {};
  const QString p = proxyPathFor(sourcePath, cacheDir);
  const QFileInfo fi(p);
  return fi.exists() && fi.size() > 0 ? p : QString();
}

int removeStaleProxies(const QString& sourcePath, const QString& cacheDir) {
  QDir dir(cacheDir);
  const QString keep = proxyFileName(sourcePath);
  int n = 0;
  for (const QString& name : dir.entryList({pathHash(sourcePath) + QStringLiteral("_*")}, QDir::Files)) {
    if (name != keep && dir.remove(name)) ++n;
  }
  return n;
}

ProxyResult makeProxy(const QString& sourcePath, const QString& cacheDir, const ProxyOptions& opt, const std::function<void(double)>& progress,
                      const std::atomic<bool>* cancel) {
  ProxyResult res;
  QElapsedTimer clock;
  clock.start();
  auto fail = [&](const QString& msg) {
    res.error = msg;
    res.seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    return res;
  };

  QString err;
  av::FormatPtr in = av::openInput(sourcePath, &err);
  if (!in) return fail(err);
  const AVCodec* decoder = nullptr;
  const int vi = av_find_best_stream(in.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (vi < 0 || !decoder) return fail(QStringLiteral("No video stream in %1").arg(sourcePath));
  AVStream* ist = in->streams[vi];

  const int srcW = ist->codecpar->width, srcH = ist->codecpar->height;
  if (srcW <= 0 || srcH <= 0) return fail(QStringLiteral("The video stream has no size"));
  if (srcW <= opt.width) {
    res.ok = true;
    res.unneeded = true;
    res.seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    return res;
  }
  const int dstW = opt.width % 2 ? opt.width + 1 : opt.width;
  const int dstH = evenInt(static_cast<double>(srcH) * dstW / srcW);

  av::CodecPtr dec(avcodec_alloc_context3(decoder));
  if (avcodec_parameters_to_context(dec.get(), ist->codecpar) < 0) return fail(QStringLiteral("Can't set up the decoder"));
  dec->pkt_timebase = ist->time_base;
  dec->thread_count = 0;
  if (const int r = avcodec_open2(dec.get(), decoder, nullptr); r < 0) return fail(QStringLiteral("Can't open the decoder: %1").arg(av::errorString(r)));

  QDir().mkpath(cacheDir);
  const QString finalPath = proxyPathFor(sourcePath, cacheDir);
  const QString partPath = finalPath + QStringLiteral(".part");
  QFile::remove(partPath);
  res.path = finalPath;

  AVFormatContext* rawOut = nullptr;
  if (avformat_alloc_output_context2(&rawOut, nullptr, "mp4", partPath.toUtf8().constData()) < 0 || !rawOut) return fail(QStringLiteral("Can't create the mp4 muxer"));
  struct OutCloser {
    AVFormatContext* ctx;
    ~OutCloser() {
      if (ctx->pb) avio_closep(&ctx->pb);
      avformat_free_context(ctx);
    }
  } outGuard{rawOut};
  if (const int r = avio_open(&rawOut->pb, partPath.toUtf8().constData(), AVIO_FLAG_WRITE); r < 0) return fail(QStringLiteral("Can't write %1: %2").arg(partPath, av::errorString(r)));

  const AVCodec* x264 = avcodec_find_encoder_by_name("libx264");
  if (!x264) return fail(QStringLiteral("This FFmpeg build has no libx264 encoder (proxies need the GPL build)"));
  av::CodecPtr enc;
  AVStream* ost = nullptr;
  av::SwsPtr sws(sws_alloc_context());
  sws->flags = SWS_BICUBIC | SWS_ACCURATE_RND;
  sws->threads = 0;
  av::FramePtr yuv(av_frame_alloc());
  av::FramePtr frame(av_frame_alloc());
  av::PacketPtr pkt(av_packet_alloc());
  av::PacketPtr outPkt(av_packet_alloc());
  const double durationSec = in->duration > 0 ? static_cast<double>(in->duration) / AV_TIME_BASE : 0;
  qint64 frames = 0;
  QString failure;

  auto drain = [&]() -> bool {
    for (;;) {
      const int r = avcodec_receive_packet(enc.get(), outPkt.get());
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) {
        failure = QStringLiteral("Encoder error: %1").arg(av::errorString(r));
        return false;
      }
      av_packet_rescale_ts(outPkt.get(), enc->time_base, ost->time_base);
      outPkt->stream_index = ost->index;
      if (const int w = av_interleaved_write_frame(rawOut, outPkt.get()); w < 0) {
        failure = QStringLiteral("Can't write the proxy: %1").arg(av::errorString(w));
        return false;
      }
    }
  };

  // creates the encoder + header on the first decoded frame, when its colour tags are known
  auto startEncoder = [&](const AVFrame* f) -> bool {
    enc.reset(avcodec_alloc_context3(x264));
    enc->width = dstW;
    enc->height = dstH;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = ist->time_base;
    AVRational fr = av_guess_frame_rate(in.get(), ist, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = {30, 1};
    enc->framerate = fr;
    enc->gop_size = 1; // all-intra
    enc->max_b_frames = 0;
    enc->sample_aspect_ratio = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio : AVRational{1, 1};
    enc->color_range = f->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    enc->colorspace = f->colorspace == AVCOL_SPC_RGB ? AVCOL_SPC_UNSPECIFIED : f->colorspace;
    enc->color_primaries = f->color_primaries;
    enc->color_trc = f->color_trc;
    enc->thread_count = 0;
    if (rawOut->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* o = nullptr;
    av_dict_set(&o, "crf", QByteArray::number(opt.crf).constData(), 0);
    av_dict_set(&o, "preset", opt.preset.toUtf8().constData(), 0);
    const int r = avcodec_open2(enc.get(), x264, &o);
    av_dict_free(&o);
    if (r < 0) {
      failure = QStringLiteral("Can't open libx264: %1").arg(av::errorString(r));
      return false;
    }
    ost = avformat_new_stream(rawOut, nullptr);
    if (!ost || avcodec_parameters_from_context(ost->codecpar, enc.get()) < 0) {
      failure = QStringLiteral("Can't create the proxy stream");
      return false;
    }
    ost->time_base = enc->time_base;
    ost->avg_frame_rate = fr;
    // keep the orientation the source is stored with
    if (const AVPacketSideData* sd = av_packet_side_data_get(ist->codecpar->coded_side_data, ist->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
      if (AVPacketSideData* nsd = av_packet_side_data_new(&ost->codecpar->coded_side_data, &ost->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX, sd->size, 0))
        std::memcpy(nsd->data, sd->data, sd->size);
    }
    AVDictionary* mux = nullptr;
    av_dict_set(&mux, "movflags", "+faststart", 0);
    const int w = avformat_write_header(rawOut, &mux);
    av_dict_free(&mux);
    if (w < 0) {
      failure = QStringLiteral("Can't write the proxy header: %1").arg(av::errorString(w));
      return false;
    }
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = dstW;
    yuv->height = dstH;
    return av_frame_get_buffer(yuv.get(), 0) >= 0;
  };

  auto encodeFrame = [&](AVFrame* f) -> bool {
    if (!enc && !startEncoder(f)) return false;
    yuv->colorspace = enc->colorspace;
    yuv->color_range = enc->color_range;
    yuv->color_primaries = enc->color_primaries;
    yuv->color_trc = enc->color_trc;
    if (av_frame_make_writable(yuv.get()) < 0) return false;
    const int r = sws_scale_frame(sws.get(), yuv.get(), f);
    if (r < 0) {
      failure = QStringLiteral("Scaling failed: %1").arg(av::errorString(r));
      return false;
    }
    yuv->pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    yuv->pict_type = AV_PICTURE_TYPE_NONE;
    if (const int s = avcodec_send_frame(enc.get(), yuv.get()); s < 0) {
      failure = QStringLiteral("Encoder rejected a frame: %1").arg(av::errorString(s));
      return false;
    }
    ++frames;
    if (progress && durationSec > 0 && (frames % 8) == 0) {
      const double t = static_cast<double>(yuv->pts) * av_q2d(ist->time_base) - (ist->start_time != AV_NOPTS_VALUE ? static_cast<double>(ist->start_time) * av_q2d(ist->time_base) : 0.0);
      progress(std::clamp(t / durationSec, 0.0, 0.999));
    }
    return drain();
  };

  auto decodeLoop = [&](AVPacket* p) -> bool { // p == nullptr flushes
    if (const int s = avcodec_send_packet(dec.get(), p); s < 0 && s != AVERROR_EOF) {
      failure = QStringLiteral("Decoder error: %1").arg(av::errorString(s));
      return false;
    }
    for (;;) {
      const int r = avcodec_receive_frame(dec.get(), frame.get());
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) {
        failure = QStringLiteral("Decoder error: %1").arg(av::errorString(r));
        return false;
      }
      const bool good = encodeFrame(frame.get());
      av_frame_unref(frame.get());
      if (!good) return false;
    }
  };

  bool cancelled = false;
  bool good = true;
  for (;;) {
    if (cancel && cancel->load()) {
      cancelled = true;
      break;
    }
    const int r = av_read_frame(in.get(), pkt.get());
    if (r < 0) break; // EOF (or a read error: what was decoded so far is flushed)
    if (pkt->stream_index == vi) good = decodeLoop(pkt.get());
    av_packet_unref(pkt.get());
    if (!good) break;
  }
  if (good && !cancelled) good = decodeLoop(nullptr);
  if (good && !cancelled && enc) {
    avcodec_send_frame(enc.get(), nullptr);
    good = drain();
    if (good) {
      if (const int t = av_write_trailer(rawOut); t < 0) {
        failure = QStringLiteral("Can't finish the proxy: %1").arg(av::errorString(t));
        good = false;
      }
    }
  }
  if (good && !cancelled && !enc) {
    failure = QStringLiteral("The video stream has no decodable frames");
    good = false;
  }
  avio_closep(&rawOut->pb);
  if (!good || cancelled) {
    QFile::remove(partPath);
    res.cancelled = cancelled;
    res.error = cancelled ? QString() : failure;
    res.seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    return res;
  }
  QFile::remove(finalPath);
  if (!QFile::rename(partPath, finalPath)) {
    QFile::remove(partPath);
    return fail(QStringLiteral("Can't move the proxy into place: %1").arg(finalPath));
  }
  removeStaleProxies(sourcePath, cacheDir);
  if (progress) progress(1.0);
  res.ok = true;
  res.seconds = static_cast<double>(clock.elapsed()) / 1000.0;
  return res;
}

} // namespace sf::xport
