#include "media/video_decoder.h"

#include "media/frame_convert.h"
#include "media/hwdevice.h"
#include "media/packet_index.h"

#include <QThread>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sf {

namespace {

// Picks D3D11 surfaces when the decoder offers them. Otherwise the first software format: that is
// what makes unsupported profiles (4:2:2 H.264, 10-bit on old GPUs) fall back by themselves.
AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_D3D11) return *p;
  }
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
  }
  return AV_PIX_FMT_NONE;
}

enum class Run { Ok, Cancelled, Failed, Restart, LandedLate };

} // namespace

struct VideoDecoder::Impl {
  QString path;
  VideoOpenOptions options;
  av::FormatPtr fmt;
  int stream = -1;
  AVStream* st = nullptr;
  const AVCodec* codec = nullptr;
  av::CodecPtr ctx;
  bool hwWanted = false;  // a D3D11VA session was requested for the current codec context
  bool hwSeen = false;    // the last decoded frame really was a D3D11 surface
  QString hwNote;         // why we are on software though hardware was possible

  std::vector<media::FrameEntry> table;
  std::vector<qint64> keys; // indices of keyframes, ascending
  VideoStreamInfo info;
  double containerStart = 0;

  av::PacketPtr pkt{av_packet_alloc()};
  av::FramePtr frame{av_frame_alloc()};
  av::FramePtr swFrame{av_frame_alloc()};
  media::FrameConverter converter;

  // Decoder position. `last` is the index of the last frame the decoder emitted since the current
  // seek (keyframe-1 right after one); only meaningful while `positioned`.
  bool positioned = false;
  bool draining = false;
  bool eof = false;
  // Streams without per-frame PTS (see PacketScan::synthesized): frames are numbered by output order
  // from the keyframe the demuxer actually landed on.
  bool sequential = false;
  bool seqPending = false; // next output is the first after a seek
  bool needLanded = false; // capture the pts of the first packet read after a seek
  int64_t seekPts = AV_NOPTS_VALUE;
  int64_t landedPts = AV_NOPTS_VALUE;
  int64_t gridHalf = 0;
  qint64 last = -1;
  VideoFramePtr lastFrame;
  QString error;
  Stats stats;

  bool openCodec(bool hw);
  bool seekToKey(qint64 keyIndex);
  VideoFramePtr makeFrame(qint64 index);
  Run run(qint64 target, qint64 keepFrom, const FrameSink& sink, const Cancel& cancel, bool afterSeek,
          VideoFramePtr* out);
  double ptsSec(qint64 index) const { return table[static_cast<size_t>(index)].pts * av_q2d(st->time_base) - containerStart; }
  qint64 keyAtOrBefore(qint64 index) const {
    const auto it = std::upper_bound(keys.begin(), keys.end(), index);
    return it == keys.begin() ? -1 : *(it - 1);
  }
};

bool VideoDecoder::Impl::openCodec(bool hw) {
  av::CodecPtr c(avcodec_alloc_context3(codec));
  if (!c || avcodec_parameters_to_context(c.get(), st->codecpar) < 0) {
    error = QStringLiteral("Can't set up decoder for %1").arg(path);
    return false;
  }
  c->pkt_timebase = st->time_base;
  if (hw) {
    av::BufferPtr dev = media::sharedD3D11Device();
    if (!dev) {
      error = QStringLiteral("D3D11VA device unavailable");
      return false;
    }
    c->hw_device_ctx = av_buffer_ref(dev.get());
    c->get_format = pickFormat;
    c->thread_count = 1; // the GPU does the work; software threads would only add latency
  } else {
    c->thread_count = options.swThreads > 0 ? options.swThreads : std::min(8, QThread::idealThreadCount());
    c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  }
  if (const int rc = avcodec_open2(c.get(), codec, nullptr); rc < 0) {
    error = QStringLiteral("Can't open %1 decoder: %2").arg(QString::fromUtf8(codec->name), av::errorString(rc));
    return false;
  }
  ctx = std::move(c);
  hwWanted = hw;
  hwSeen = false;
  positioned = false;
  draining = eof = false;
  last = -1;
  lastFrame.reset();
  return true;
}

bool VideoDecoder::Impl::seekToKey(qint64 keyIndex) {
  const int64_t ts = table[static_cast<size_t>(std::max<qint64>(keyIndex, 0))].pts;
  const int64_t pos = keyIndex <= 0 ? std::max<int64_t>(table[0].pos, 0) : table[static_cast<size_t>(keyIndex)].pos;
  // Without per-frame PTS a timestamp seek can land anywhere; the keyframe's byte offset is exact.
  int rc = sequential && pos >= 0 ? av_seek_frame(fmt.get(), stream, pos, AVSEEK_FLAG_BYTE) : -1;
  if (rc < 0) rc = av_seek_frame(fmt.get(), stream, ts, AVSEEK_FLAG_BACKWARD);
  // Some demuxers refuse a seek to the very first timestamp; reading from the top is what we want anyway.
  if (rc < 0) rc = av_seek_frame(fmt.get(), -1, fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0, AVSEEK_FLAG_BACKWARD);
  if (rc < 0) {
    error = QStringLiteral("Seek failed in %1: %2").arg(path, av::errorString(rc));
    return false;
  }
  avcodec_flush_buffers(ctx.get());
  positioned = true;
  draining = eof = false;
  last = keyIndex - 1;
  seqPending = needLanded = true;
  seekPts = ts;
  landedPts = AV_NOPTS_VALUE;
  ++stats.seeks;
  return true;
}

VideoFramePtr VideoDecoder::Impl::makeFrame(qint64 index) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Timer {
    double& acc;
    std::chrono::steady_clock::time_point t0;
    ~Timer() { acc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
  } timer{stats.convertSeconds, t0};
  AVFrame* src = frame.get();
  if (frame->format == AV_PIX_FMT_D3D11) {
    av_frame_unref(swFrame.get());
    if (const int rc = av_hwframe_transfer_data(swFrame.get(), frame.get(), 0); rc < 0) {
      error = QStringLiteral("GPU download failed: %1").arg(av::errorString(rc));
      return nullptr;
    }
    av_frame_copy_props(swFrame.get(), frame.get());
    src = swFrame.get();
  }
  QImage image = converter.toRgba(src);
  if (image.isNull()) {
    error = QStringLiteral("Unsupported pixel format %1").arg(av_get_pix_fmt_name(static_cast<AVPixelFormat>(src->format)));
    return nullptr;
  }
  ++stats.framesConverted;
  auto out = std::make_shared<VideoFrame>();
  out->index = index;
  out->ptsSec = ptsSec(index);
  out->image = std::move(image);
  out->hardware = hwSeen;
  return out;
}

Run VideoDecoder::Impl::run(qint64 target, qint64 keepFrom, const FrameSink& sink, const Cancel& cancel, bool afterSeek,
                            VideoFramePtr* out) {
  bool first = true;
  while (true) {
    if (cancel && cancel()) return Run::Cancelled;
    const int rc = avcodec_receive_frame(ctx.get(), frame.get());
    if (rc == 0) {
      ++stats.framesDecoded;
      hwSeen = frame->format == AV_PIX_FMT_D3D11;
      if (hwWanted && !hwSeen) {
        if (options.hw == HwMode::Required) {
          error = QStringLiteral("Hardware decoding required but this stream is decoded in software");
          return Run::Failed;
        }
        hwNote = QStringLiteral("stream not supported by D3D11VA");
        av_frame_unref(frame.get());
        return Run::Restart;
      }
      const int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
      qint64 idx;
      if (sequential) {
        if (seqPending) {
          idx = std::max<int64_t>(0, media::frameIndexForPts(table, (landedPts != AV_NOPTS_VALUE ? landedPts : seekPts) + gridHalf));
          seqPending = false;
        } else {
          idx = last + 1;
        }
        if (idx >= info.frameCount) idx = -1;
      } else {
        idx = pts == AV_NOPTS_VALUE ? last + 1 : media::frameIndexForPts(table, pts);
      }
      if (idx < 0) {
        av_frame_unref(frame.get());
        continue;
      }
      if (first && afterSeek && idx > target) {
        av_frame_unref(frame.get());
        return Run::LandedLate; // the demuxer seeked past our keyframe
      }
      first = false;
      last = idx;
      if (idx < target) {
        if (sink && keepFrom >= 0 && idx >= keepFrom) {
          if (VideoFramePtr f = makeFrame(idx)) sink(std::move(f));
        }
        av_frame_unref(frame.get());
        continue;
      }
      *out = makeFrame(idx);
      av_frame_unref(frame.get());
      return *out ? Run::Ok : Run::Failed;
    }
    if (rc == AVERROR(EAGAIN)) {
      if (draining) {
        error = QStringLiteral("Decoder stalled");
        return Run::Failed;
      }
      const int r = av_read_frame(fmt.get(), pkt.get());
      if (r < 0) {
        draining = true;
        avcodec_send_packet(ctx.get(), nullptr);
      } else {
        if (pkt->stream_index == stream) {
          if (needLanded) {
            landedPts = pkt->pts;
            needLanded = false;
          }
          const int s = avcodec_send_packet(ctx.get(), pkt.get());
          if (s < 0 && s != AVERROR_INVALIDDATA && s != AVERROR(EAGAIN)) {
            av_packet_unref(pkt.get());
            error = QStringLiteral("Decoder rejected a packet: %1").arg(av::errorString(s));
            return hwWanted ? Run::Restart : Run::Failed;
          }
        }
        av_packet_unref(pkt.get());
      }
      continue;
    }
    if (rc == AVERROR_EOF) {
      eof = true;
      positioned = false;
      error = QStringLiteral("Stream ended before frame %1").arg(target);
      return Run::Failed;
    }
    if (rc == AVERROR_INVALIDDATA) continue; // one corrupt frame shouldn't end playback
    error = QStringLiteral("Decode error: %1").arg(av::errorString(rc));
    if (hwWanted && options.hw != HwMode::Required) {
      hwNote = QStringLiteral("D3D11VA decode error: %1").arg(av::errorString(rc));
      return Run::Restart;
    }
    return Run::Failed;
  }
}

VideoDecoder::VideoDecoder() : d(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(const QString& path, const VideoOpenOptions& options, QString* error) {
  std::unique_ptr<VideoDecoder> dec(new VideoDecoder());
  Impl& d = *dec->d;
  d.path = path;
  d.options = options;
  auto fail = [&](const QString& msg) -> std::unique_ptr<VideoDecoder> {
    if (error) *error = msg;
    return nullptr;
  };

  QString err;
  d.fmt = av::openInput(path, &err);
  if (!d.fmt) return fail(err);

  const AVCodec* codec = nullptr;
  int v = av_find_best_stream(d.fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  // cover art in mp3/m4a shows up as a one-frame video stream; it isn't video
  if (v >= 0 && (d.fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;
  if (v < 0 || !codec) return fail(QStringLiteral("%1 has no decodable video stream").arg(path));
  d.stream = v;
  d.st = d.fmt->streams[v];
  d.codec = codec;
  d.containerStart = d.fmt->start_time != AV_NOPTS_VALUE ? d.fmt->start_time / double(AV_TIME_BASE) : 0.0;

  media::PacketScan scan = media::scanVideoPackets(d.fmt.get(), v);
  if (scan.frames.empty()) return fail(QStringLiteral("%1 has no video frames").arg(path));
  d.sequential = scan.synthesized;
  d.table = std::move(scan.frames);
  if (d.table.size() > 1) d.gridHalf = (d.table[1].pts - d.table[0].pts) / 2;
  for (size_t i = 0; i < d.table.size(); ++i) {
    // without per-frame PTS only keyframes with a known byte offset can be reached exactly
    if (d.table[i].key && (!d.sequential || d.table[i].pos >= 0)) d.keys.push_back(static_cast<qint64>(i));
  }
  // The first frame is always a valid start even if the muxer forgot to flag it.
  if (d.keys.empty() || d.keys.front() != 0) d.keys.insert(d.keys.begin(), 0);
  for (unsigned i = 0; i < d.fmt->nb_streams; ++i) {
    if (static_cast<int>(i) != v) d.fmt->streams[i]->discard = AVDISCARD_ALL; // don't demux audio we won't use
  }

  const bool hwPossible = options.hw != HwMode::Off && media::codecHasD3D11(codec) && media::sharedD3D11Device();
  if (options.hw == HwMode::Required && !hwPossible) return fail(QStringLiteral("D3D11VA not available for %1").arg(QString::fromUtf8(codec->name)));
  if (options.hw != HwMode::Off && !hwPossible) d.hwNote = QStringLiteral("D3D11VA unavailable for this codec");
  if (!d.openCodec(hwPossible)) {
    if (hwPossible && options.hw == HwMode::Auto) {
      d.hwNote = d.error;
      if (!d.openCodec(false)) return fail(d.error);
    } else {
      return fail(d.error);
    }
  }

  VideoStreamInfo& info = d.info;
  info.frameCount = static_cast<qint64>(d.table.size());
  info.width = d.st->codecpar->width;
  info.height = d.st->codecpar->height;
  info.rotation = media::displayRotation(d.st->codecpar);
  info.codec = QString::fromUtf8(avcodec_get_name(d.st->codecpar->codec_id));
  if (const char* n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(d.st->codecpar->format))) info.pixelFormat = QString::fromUtf8(n);
  const double tb = av_q2d(d.st->time_base);
  info.startSec = d.ptsSec(0);
  const int64_t span = d.table.back().pts - d.table.front().pts;
  if (d.table.size() > 1 && span > 0) {
    const double frameDur = span / double(d.table.size() - 1);
    info.avgFps = 1.0 / (frameDur * tb);
    info.durationSec = d.ptsSec(static_cast<qint64>(d.table.size()) - 1) + frameDur * tb - info.startSec;
    // same jitter-tolerant rule as the probe: 2 ticks or 10 % of the mean gap
    const double tol = std::max(2.0, frameDur * 0.1);
    for (size_t i = 1; i < d.table.size() && !info.vfr; ++i) {
      info.vfr = std::abs((d.table[i].pts - d.table[i - 1].pts) - frameDur) > tol;
    }
  } else {
    info.avgFps = av_q2d(av_guess_frame_rate(d.fmt.get(), d.st, nullptr));
    info.durationSec = info.avgFps > 0 ? 1.0 / info.avgFps : 0;
  }
  return dec;
}

const VideoStreamInfo& VideoDecoder::info() const { return d->info; }
bool VideoDecoder::usingHardware() const { return d->hwWanted && d->hwSeen; }
QString VideoDecoder::lastError() const { return d->error; }
VideoDecoder::Stats VideoDecoder::stats() const { return d->stats; }

QString VideoDecoder::decoderName() const {
  QString s = d->info.codec;
  if (d->hwWanted && (d->hwSeen || !d->positioned)) return s + QStringLiteral(" (d3d11va)");
  s += QStringLiteral(" (software");
  if (!d->hwNote.isEmpty()) s += QStringLiteral(", ") + d->hwNote;
  return s + QLatin1Char(')');
}

double VideoDecoder::timeOfFrame(qint64 index) const {
  return d->ptsSec(std::clamp<qint64>(index, 0, d->info.frameCount - 1));
}

qint64 VideoDecoder::indexAtTime(double sec) const {
  const double ticks = (sec + d->containerStart) / av_q2d(d->st->time_base);
  // half a tick of slack so that t = N/fps lands on frame N despite double rounding
  const int64_t pts = static_cast<int64_t>(std::llround(ticks));
  return std::clamp<qint64>(media::frameIndexForPts(d->table, pts), 0, d->info.frameCount - 1);
}

bool VideoDecoder::isKeyFrame(qint64 index) const {
  return index >= 0 && index < d->info.frameCount && d->table[static_cast<size_t>(index)].key;
}

VideoFramePtr VideoDecoder::frameAt(qint64 index, const Cancel& cancel, qint64 keepFrom, const FrameSink& sink) {
  Impl& s = *d;
  if (index < 0 || index >= s.info.frameCount) {
    s.error = QStringLiteral("Frame %1 out of range (0..%2)").arg(index).arg(s.info.frameCount - 1);
    return nullptr;
  }
  if (s.lastFrame && s.lastFrame->index == index) return s.lastFrame;

  qint64 key = s.keyAtOrBefore(index);
  int attempt = 0;
  int restarts = 0;
  while (true) {
    // Reuse decoder state when going forward costs no more than seeking would (plus a few frames
    // for the flush and the demuxer seek itself).
    const bool forward = attempt == 0 && s.positioned && !s.eof && index > s.last &&
                         (index - s.last) <= (index - std::max<qint64>(key, 0)) + 4;
    if (!forward && !s.seekToKey(key)) return nullptr;

    VideoFramePtr out;
    switch (s.run(index, keepFrom, sink, cancel, !forward, &out)) {
    case Run::Ok:
      s.lastFrame = out;
      return out;
    case Run::Cancelled:
      return nullptr;
    case Run::Failed:
      s.positioned = false;
      return nullptr;
    case Run::Restart:
      // hardware gave up (or the stream isn't supported): finish the request in software
      if (++restarts > 1 || !s.openCodec(false)) {
        if (restarts > 1) s.error = QStringLiteral("Decoder restart failed");
        return nullptr;
      }
      attempt = 0;
      break;
    case Run::LandedLate:
      // seek landed after our keyframe; back off to earlier keyframes, finally the stream start
      ++attempt;
      if (key >= 0) key = attempt >= 3 ? -1 : s.keyAtOrBefore(key - 1);
      if (attempt > 4) {
        s.error = QStringLiteral("Can't seek to frame %1").arg(index);
        return nullptr;
      }
      break;
    }
  }
}

VideoFramePtr VideoDecoder::next(const Cancel& cancel) {
  const qint64 n = d->lastFrame ? d->lastFrame->index + 1 : 0;
  if (n >= d->info.frameCount) return nullptr;
  return frameAt(n, cancel);
}

} // namespace sf
