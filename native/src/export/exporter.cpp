#include "export/exporter.h"

#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "media/ffmpeg.h"
#include "render/offscreen.h"

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

namespace sf::xport {

namespace {

constexpr int kQueueDepth = 3;
constexpr std::int64_t kAudioChunk = 4800; // 100 ms

// Bounded hand-off between the render thread and the encode thread. Closing wakes both sides.
class FrameQueue {
public:
  bool push(QImage img) {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return closed_ || q_.size() < kQueueDepth; });
    if (closed_) return false;
    q_.push_back(std::move(img));
    cv_.notify_all();
    return true;
  }
  // false: closed and drained
  bool pop(QImage& out) {
    std::unique_lock lock(m_);
    cv_.wait(lock, [&] { return closed_ || !q_.empty(); });
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    cv_.notify_all();
    return true;
  }
  void close() {
    const std::lock_guard lock(m_);
    closed_ = true;
    cv_.notify_all();
  }
  void reopen() {
    const std::lock_guard lock(m_);
    closed_ = false;
  }

private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<QImage> q_;
  bool closed_ = false;
};

QString av_err(int code) { return av::errorString(code); }

struct Tags {
  AVColorPrimaries primaries = AVCOL_PRI_BT709;
  AVColorTransferCharacteristic trc = AVCOL_TRC_BT709;
  AVColorSpace matrix = AVCOL_SPC_BT709;
};

Tags tagsFor(const ExportPlan& p) {
  Tags t;
  if (p.primaries == QLatin1String("bt2020")) t.primaries = AVCOL_PRI_BT2020;
  if (p.transfer == QLatin1String("smpte2084")) t.trc = AVCOL_TRC_SMPTE2084;
  else if (p.transfer == QLatin1String("arib-std-b67")) t.trc = AVCOL_TRC_ARIB_STD_B67;
  if (p.matrix == QLatin1String("bt2020nc")) t.matrix = AVCOL_SPC_BT2020_NCL;
  return t;
}

AVPixelFormat pixelFormatFor(const ExportPlan& p, const ExportSettings& s, bool nvenc) {
  const bool ten = p.bits > 8;
  switch (p.video) {
    case VideoCodec::H264:
    case VideoCodec::H265:
      if (nvenc) return ten ? AV_PIX_FMT_P010LE : AV_PIX_FMT_YUV420P;
      return ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    case VideoCodec::ProRes: return s.proresProfile == ProResProfile::P4444 ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV422P10LE;
    case VideoCodec::DnxHr:
      if (s.dnxProfile == QLatin1String("dnxhr_444")) return AV_PIX_FMT_YUV444P10LE;
      if (s.dnxProfile == QLatin1String("dnxhr_hqx")) return AV_PIX_FMT_YUV422P10LE;
      return AV_PIX_FMT_YUV422P;
    case VideoCodec::None: break;
  }
  return AV_PIX_FMT_YUV420P;
}

class Job {
public:
  Job(const TimelineDoc& doc, const render::AssetTable& assets, const ExportSettings& s, const ExportPlan& p, const ProgressFn& progress,
      const std::atomic<bool>* cancel)
      : doc_(doc), assets_(assets), s_(s), p_(p), progress_(progress), cancel_(cancel) {}

  ExportResult run();

private:
  bool cancelled() const { return cancel_ && cancel_->load(); }
  void fail(const QString& msg) {
    {
      const std::lock_guard lock(errM_);
      if (error_.isEmpty()) error_ = msg;
    }
    failed_ = true;
    queue_.close();
  }
  bool stop() const { return failed_ || cancelled(); }

  bool openOutput();
  bool openVideoEncoder();
  bool tryOpenVideo(const AVCodec* codec, bool nvenc);
  bool openAudioEncoder();
  bool measureLoudness();
  bool convertAndEncode(const QImage& img, qint64 index);
  bool feedAudio(std::int64_t upTo, bool finalChunk);
  bool sendAudioFrames(bool flush);
  bool drain(AVCodecContext* c, AVStream* st);
  bool encodeLoop();
  void renderLoop();
  void report(qint64 frame, const QString& stage, bool force = false);
  void abandon();

  const TimelineDoc& doc_;
  const render::AssetTable& assets_;
  const ExportSettings& s_;
  ExportPlan p_;
  const ProgressFn& progress_;
  const std::atomic<bool>* cancel_;

  Tags tags_;
  QString tmpPath_;
  AVFormatContext* fmt_ = nullptr;
  bool headerWritten_ = false;
  av::CodecPtr venc_, aenc_;
  AVStream *vst_ = nullptr, *ast_ = nullptr;
  av::SwsPtr sws_;
  av::FramePtr yuv_;
  av::PacketPtr pkt_;
  QString videoEncoderName_;
  QString audioEncoderName_;
  bool nvenc_ = false;

  // audio
  std::unique_ptr<audio::AudioEngine> engine_;
  std::vector<float> pending_; // interleaved, gain applied, not yet encoded
  std::int64_t audioRendered_ = 0; // samples rendered from the engine (relative to the range start)
  std::int64_t audioEncoded_ = 0;  // samples handed to the encoder (pts of the next frame)
  float gain_ = 1.0f;
  double gainDb_ = 0;
  av::FramePtr aframe_;
  int audioFrameSize_ = 0;

  FrameQueue queue_;
  std::mutex errM_;
  QString error_;
  std::atomic<bool> failed_{false};
  std::atomic<qint64> missing_{0};
  std::atomic<qint64> framesDone_{0};
  QElapsedTimer clock_;
  qint64 lastReportMs_ = -1000;
  std::mutex reportM_;
};

void Job::report(qint64 frame, const QString& stage, bool force) {
  if (!progress_) return;
  const std::lock_guard lock(reportM_);
  const qint64 now = clock_.elapsed();
  if (!force && now - lastReportMs_ < 100) return;
  lastReportMs_ = now;
  ExportProgress pr;
  pr.frame = frame;
  pr.totalFrames = p_.frames;
  const double sec = static_cast<double>(now) / 1000.0;
  pr.fps = sec > 0.05 ? static_cast<double>(frame) / sec : 0;
  pr.fraction = p_.frames > 0 ? std::min(1.0, static_cast<double>(frame) / static_cast<double>(p_.frames)) : 0;
  pr.etaSec = pr.fps > 0 ? static_cast<double>(p_.frames - frame) / pr.fps : 0;
  pr.stage = stage;
  progress_(pr);
}

bool Job::openOutput() {
  const QString finalPath = QFileInfo(s_.outputPath).absoluteFilePath();
  QDir().mkpath(QFileInfo(finalPath).absolutePath());
  tmpPath_ = finalPath + QStringLiteral(".sfpart");
  const char* format = "mp4";
  switch (p_.container) {
    case Container::Mp4: format = "mp4"; break;
    case Container::Mov: format = "mov"; break;
    case Container::Mkv: format = "matroska"; break;
    case Container::Wav: format = "wav"; break;
    case Container::Auto: break;
  }
  AVFormatContext* raw = nullptr;
  const int r = avformat_alloc_output_context2(&raw, nullptr, format, tmpPath_.toUtf8().constData());
  if (r < 0 || !raw) {
    fail(QStringLiteral("Can't create the %1 muxer: %2").arg(QString::fromLatin1(format), av_err(r)));
    return false;
  }
  fmt_ = raw;
  const int o = avio_open(&fmt_->pb, tmpPath_.toUtf8().constData(), AVIO_FLAG_WRITE);
  if (o < 0) {
    fail(QStringLiteral("Can't write %1: %2").arg(QDir::toNativeSeparators(finalPath), av_err(o)));
    return false;
  }
  return true;
}

bool Job::tryOpenVideo(const AVCodec* codec, bool nvenc) {
  av::CodecPtr c(avcodec_alloc_context3(codec));
  if (!c) return false;
  c->width = p_.width;
  c->height = p_.height;
  c->time_base = {1, p_.fps};
  c->framerate = {p_.fps, 1};
  c->pkt_timebase = c->time_base;
  c->sample_aspect_ratio = {1, 1};
  c->pix_fmt = pixelFormatFor(p_, s_, nvenc);
  c->color_range = AVCOL_RANGE_MPEG;
  c->colorspace = tags_.matrix;
  c->color_primaries = tags_.primaries;
  c->color_trc = tags_.trc;
  c->thread_count = 0;
  if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  const bool bitrateMode = s_.videoBitrateK > 0;
  if (bitrateMode && (p_.video == VideoCodec::H264 || p_.video == VideoCodec::H265)) {
    c->bit_rate = static_cast<int64_t>(s_.videoBitrateK) * 1000;
    c->rc_max_rate = c->bit_rate * 3 / 2;
    c->rc_buffer_size = static_cast<int>(c->bit_rate);
  }
  AVDictionary* opts = nullptr;
  auto set = [&](const char* k, const QString& v) { av_dict_set(&opts, k, v.toUtf8().constData(), 0); };
  switch (p_.video) {
    case VideoCodec::H264:
    case VideoCodec::H265: {
      if (nvenc) {
        set("preset", s_.encoderPreset.isEmpty() ? QStringLiteral("p5") : s_.encoderPreset);
        if (!bitrateMode) {
          set("rc", QStringLiteral("vbr"));
          set("cq", QString::number(s_.crf >= 0 ? s_.crf : 23));
        }
        if (p_.bits > 8) set("profile", QStringLiteral("main10"));
      } else {
        static const QRegularExpression nvPreset(QStringLiteral("^p[1-7]$"));
        set("preset", s_.encoderPreset.isEmpty() || nvPreset.match(s_.encoderPreset).hasMatch() ? QStringLiteral("medium") : s_.encoderPreset);
        if (!bitrateMode) set("crf", QString::number(s_.crf >= 0 ? s_.crf : (p_.video == VideoCodec::H264 ? 20 : 23)));
        if (p_.video == VideoCodec::H265) {
          QString x = QStringLiteral("log-level=error");
          if (p_.transfer == QLatin1String("smpte2084")) {
            x += QStringLiteral(":repeat-headers=1:master-display=G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)L(10000000,1):max-cll=1000,400");
          }
          set("x265-params", x);
        }
      }
      break;
    }
    case VideoCodec::ProRes:
      set("profile", QString::number(static_cast<int>(s_.proresProfile)));
      set("vendor", QStringLiteral("apl0"));
      break;
    case VideoCodec::DnxHr: set("profile", s_.dnxProfile); break;
    case VideoCodec::None: break;
  }
  const int r = avcodec_open2(c.get(), codec, &opts);
  av_dict_free(&opts);
  if (r < 0) {
    fail(QStringLiteral("Can't open the %1 encoder (%2x%3, %4 bit): %5")
             .arg(QString::fromLatin1(codec->name)).arg(p_.width).arg(p_.height).arg(p_.bits).arg(av_err(r)));
    return false;
  }
  venc_ = std::move(c);
  videoEncoderName_ = QString::fromLatin1(codec->name);
  nvenc_ = nvenc;
  return true;
}

bool Job::openVideoEncoder() {
  tags_ = tagsFor(p_);
  bool opened = false;
  if (s_.hardware == Hardware::Auto && (p_.video == VideoCodec::H264 || p_.video == VideoCodec::H265)) {
    if (hardwareEncoderUsable(p_.video)) {
      const AVCodec* hw = avcodec_find_encoder_by_name(p_.video == VideoCodec::H264 ? "h264_nvenc" : "hevc_nvenc");
      if (hw && !(p_.video == VideoCodec::H264 && p_.bits > 8)) {
        opened = tryOpenVideo(hw, true);
        if (!opened) {
          // fall back to software: forget the NVENC error
          const std::lock_guard lock(errM_);
          error_.clear();
          failed_ = false;
          queue_.reopen();
          p_.warnings << QStringLiteral("NVENC failed to open; encoding in software");
        }
      }
    } else {
      p_.warnings << QStringLiteral("No usable NVENC encoder on this machine; encoding in software");
    }
  }
  if (!opened) {
    const AVCodec* sw = avcodec_find_encoder_by_name(p_.videoEncoder.toUtf8().constData());
    if (!sw) {
      fail(QStringLiteral("This FFmpeg build has no %1 encoder").arg(p_.videoEncoder));
      return false;
    }
    if (!tryOpenVideo(sw, false)) return false;
  }
  vst_ = avformat_new_stream(fmt_, nullptr);
  if (!vst_) {
    fail(QStringLiteral("Out of memory creating the video stream"));
    return false;
  }

  // HDR10 static metadata (PQ only): mastering display primaries/luminance and content light level, so
  // players and the muxer (mkv, mp4 mdcv/clli) carry them. The values describe a 1000-nit BT.2020 master.
  if (p_.transfer == QLatin1String("smpte2084")) {
    size_t sz = 0;
    if (AVMasteringDisplayMetadata* md = av_mastering_display_metadata_alloc_size(&sz)) {
      const int d = 50000;
      md->display_primaries[0][0] = av_make_q(35400, d); // R
      md->display_primaries[0][1] = av_make_q(14600, d);
      md->display_primaries[1][0] = av_make_q(8500, d); // G
      md->display_primaries[1][1] = av_make_q(39850, d);
      md->display_primaries[2][0] = av_make_q(6550, d); // B
      md->display_primaries[2][1] = av_make_q(2300, d);
      md->white_point[0] = av_make_q(15635, d);
      md->white_point[1] = av_make_q(16450, d);
      md->has_primaries = 1;
      md->max_luminance = av_make_q(1000, 1);
      md->min_luminance = av_make_q(1, 10000);
      md->has_luminance = 1;
      if (!av_packet_side_data_add(&venc_->coded_side_data, &venc_->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, md, sz, 0)) av_free(md);
    }
    if (AVContentLightMetadata* cl = av_content_light_metadata_alloc(&sz)) {
      cl->MaxCLL = 1000;
      cl->MaxFALL = 400;
      if (!av_packet_side_data_add(&venc_->coded_side_data, &venc_->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, cl, sz, 0)) av_free(cl);
    }
  }
  const int r = avcodec_parameters_from_context(vst_->codecpar, venc_.get());
  if (r < 0) {
    fail(QStringLiteral("Can't describe the video stream: %1").arg(av_err(r)));
    return false;
  }
  vst_->time_base = venc_->time_base;
  vst_->avg_frame_rate = {p_.fps, 1};
  vst_->r_frame_rate = {p_.fps, 1};

  // RGB -> YUV (+ scale) context; matrix and range come from the frame tags
  sws_.reset(sws_alloc_context());
  if (!sws_) {
    fail(QStringLiteral("Out of memory creating the colour converter"));
    return false;
  }
  sws_->flags = SWS_BICUBIC;
  sws_->threads = 0;
  yuv_.reset(av_frame_alloc());
  yuv_->format = venc_->pix_fmt;
  yuv_->width = p_.width;
  yuv_->height = p_.height;
  yuv_->colorspace = tags_.matrix;
  yuv_->color_range = AVCOL_RANGE_MPEG;
  yuv_->color_primaries = tags_.primaries;
  yuv_->color_trc = tags_.trc;
  yuv_->sample_aspect_ratio = {1, 1};
  if (av_frame_get_buffer(yuv_.get(), 0) < 0) {
    fail(QStringLiteral("Out of memory allocating the picture buffer"));
    return false;
  }
  return true;
}

bool Job::openAudioEncoder() {
  const char* name = "aac";
  AVSampleFormat fmt = AV_SAMPLE_FMT_FLTP;
  switch (p_.audio) {
    case AudioCodec::Aac: break;
    case AudioCodec::Pcm16: name = "pcm_s16le"; fmt = AV_SAMPLE_FMT_S16; break;
    case AudioCodec::Pcm24: name = "pcm_s24le"; fmt = AV_SAMPLE_FMT_S32; break;
    case AudioCodec::Pcm32f: name = "pcm_f32le"; fmt = AV_SAMPLE_FMT_FLT; break;
    case AudioCodec::None: return true;
  }
  const AVCodec* codec = avcodec_find_encoder_by_name(name);
  if (!codec) {
    fail(QStringLiteral("This FFmpeg build has no %1 encoder").arg(QString::fromLatin1(name)));
    return false;
  }
  av::CodecPtr c(avcodec_alloc_context3(codec));
  c->sample_fmt = fmt;
  c->sample_rate = audio::kRate;
  av_channel_layout_default(&c->ch_layout, 2);
  c->time_base = {1, audio::kRate};
  if (p_.audio == AudioCodec::Aac) c->bit_rate = static_cast<int64_t>(s_.audioBitrateK) * 1000;
  if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  const int r = avcodec_open2(c.get(), codec, nullptr);
  if (r < 0) {
    fail(QStringLiteral("Can't open the %1 encoder: %2").arg(QString::fromLatin1(name), av_err(r)));
    return false;
  }
  ast_ = avformat_new_stream(fmt_, nullptr);
  if (!ast_ || avcodec_parameters_from_context(ast_->codecpar, c.get()) < 0) {
    fail(QStringLiteral("Can't create the audio stream"));
    return false;
  }
  ast_->time_base = c->time_base;
  audioFrameSize_ = c->frame_size; // 1024 for AAC, 0 (any) for PCM
  aenc_ = std::move(c);
  audioEncoderName_ = QString::fromLatin1(name);
  return true;
}

bool Job::measureLoudness() {
  report(0, QStringLiteral("measuring loudness"), true);
  auto sources = std::make_shared<audio::FileProvider>();
  std::map<QString, audio::FileProvider::File> files;
  for (const auto& [id, ref] : assets_) files[id] = {ref.path, ref.kind != AssetKind::Image};
  sources->setFiles(std::move(files));
  auto docPtr = std::make_shared<const TimelineDoc>(doc_);
  audio::AudioEngine engine(docPtr, sources);
  audio::LoudnessMeter meter;
  const std::int64_t base = audio::frameToSample(p_.in, p_.fps);
  std::vector<float> buf(static_cast<size_t>(kAudioChunk) * 2);
  for (std::int64_t done = 0; done < p_.audioSamples; done += kAudioChunk) {
    if (stop()) return false;
    const std::int64_t n = std::min<std::int64_t>(kAudioChunk, p_.audioSamples - done);
    engine.render(base + done, n, buf.data());
    meter.process(buf.data(), n);
  }
  audio::LoudnessResult r;
  r.integrated = meter.integrated();
  r.range = meter.loudnessRange();
  r.truePeakDb = meter.truePeakDb();
  r.samplePeakDb = meter.samplePeakDb();
  gainDb_ = audio::normalizeGainDb(r, *s_.loudnessLufs, s_.truePeakCeilingDb);
  if (!std::isfinite(gainDb_)) gainDb_ = 0;
  gain_ = static_cast<float>(std::pow(10.0, gainDb_ / 20.0));
  return true;
}

bool Job::drain(AVCodecContext* c, AVStream* st) {
  for (;;) {
    const int r = avcodec_receive_packet(c, pkt_.get());
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
    if (r < 0) {
      fail(QStringLiteral("Encoder error: %1").arg(av_err(r)));
      return false;
    }
    av_packet_rescale_ts(pkt_.get(), c->time_base, st->time_base);
    pkt_->stream_index = st->index;
    const int w = av_interleaved_write_frame(fmt_, pkt_.get());
    if (w < 0) {
      fail(QStringLiteral("Can't write to the file: %1").arg(av_err(w)));
      return false;
    }
  }
}

bool Job::convertAndEncode(const QImage& img, qint64 index) {
  av::FramePtr src(av_frame_alloc());
  src->format = img.format() == QImage::Format_RGBA64 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
  src->width = img.width();
  src->height = img.height();
  src->data[0] = const_cast<uint8_t*>(img.constBits());
  src->linesize[0] = static_cast<int>(img.bytesPerLine());
  src->colorspace = AVCOL_SPC_RGB;
  src->color_range = AVCOL_RANGE_JPEG;
  src->color_primaries = tags_.primaries;
  src->color_trc = tags_.trc;
  if (av_frame_make_writable(yuv_.get()) < 0) {
    fail(QStringLiteral("Out of memory allocating the picture buffer"));
    return false;
  }
  const int r = sws_scale_frame(sws_.get(), yuv_.get(), src.get());
  if (r < 0) {
    fail(QStringLiteral("Colour conversion failed: %1").arg(av_err(r)));
    return false;
  }
  yuv_->pts = index;
  yuv_->pict_type = AV_PICTURE_TYPE_NONE;
  const int sres = avcodec_send_frame(venc_.get(), yuv_.get());
  if (sres < 0) {
    fail(QStringLiteral("Encoder rejected frame %1: %2").arg(index).arg(av_err(sres)));
    return false;
  }
  return drain(venc_.get(), vst_);
}

// Hands whole encoder frames from `pending_` to the encoder; with `flush` also the short tail.
bool Job::sendAudioFrames(bool flush) {
  const int ch = 2;
  for (;;) {
    const std::int64_t avail = static_cast<std::int64_t>(pending_.size() / ch);
    const std::int64_t want = audioFrameSize_ > 0 ? audioFrameSize_ : kAudioChunk;
    std::int64_t n = want;
    if (avail < want) {
      if (!flush || avail == 0) return true;
      n = avail;
    }
    av::FramePtr f(av_frame_alloc());
    f->format = aenc_->sample_fmt;
    f->sample_rate = audio::kRate;
    f->nb_samples = static_cast<int>(n);
    av_channel_layout_default(&f->ch_layout, 2);
    if (av_frame_get_buffer(f.get(), 0) < 0) {
      fail(QStringLiteral("Out of memory allocating audio"));
      return false;
    }
    const float* in = pending_.data();
    switch (aenc_->sample_fmt) {
      case AV_SAMPLE_FMT_FLTP: {
        auto* l = reinterpret_cast<float*>(f->data[0]);
        auto* r = reinterpret_cast<float*>(f->data[1]);
        for (std::int64_t i = 0; i < n; ++i) {
          l[i] = in[i * 2];
          r[i] = in[i * 2 + 1];
        }
        break;
      }
      case AV_SAMPLE_FMT_FLT: std::memcpy(f->data[0], in, static_cast<size_t>(n) * 2 * sizeof(float)); break;
      case AV_SAMPLE_FMT_S16: {
        auto* o = reinterpret_cast<int16_t*>(f->data[0]);
        for (std::int64_t i = 0; i < n * 2; ++i) {
          const double v = std::nearbyint(static_cast<double>(in[i]) * 32768.0);
          o[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
        }
        break;
      }
      case AV_SAMPLE_FMT_S32: { // pcm_s24le takes the 24 bits left-justified in 32
        auto* o = reinterpret_cast<int32_t*>(f->data[0]);
        for (std::int64_t i = 0; i < n * 2; ++i) {
          const double v = std::nearbyint(static_cast<double>(in[i]) * 8388608.0);
          o[i] = static_cast<int32_t>(std::clamp(v, -8388608.0, 8388607.0)) * 256;
        }
        break;
      }
      default: break;
    }
    f->pts = audioEncoded_;
    audioEncoded_ += n;
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n * ch));
    const int sres = avcodec_send_frame(aenc_.get(), f.get());
    if (sres < 0) {
      fail(QStringLiteral("Audio encoder rejected a frame: %1").arg(av_err(sres)));
      return false;
    }
    if (!drain(aenc_.get(), ast_)) return false;
  }
}

// Renders audio up to sample `upTo` (relative to the range start) and encodes what is complete.
bool Job::feedAudio(std::int64_t upTo, bool finalChunk) {
  if (!aenc_) return true;
  upTo = std::min(upTo, p_.audioSamples);
  const std::int64_t base = audio::frameToSample(p_.in, p_.fps);
  std::vector<float> buf(static_cast<size_t>(kAudioChunk) * 2);
  while (audioRendered_ < upTo) {
    if (stop()) return false;
    const std::int64_t n = std::min<std::int64_t>(kAudioChunk, upTo - audioRendered_);
    engine_->render(base + audioRendered_, n, buf.data());
    audioRendered_ += n;
    if (gain_ != 1.0f)
      for (std::int64_t i = 0; i < n * 2; ++i) buf[static_cast<size_t>(i)] *= gain_;
    pending_.insert(pending_.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n * 2));
    if (!sendAudioFrames(false)) return false;
  }
  if (finalChunk) return sendAudioFrames(true);
  return true;
}

bool Job::encodeLoop() {
  if (venc_) {
    for (qint64 i = 0; i < p_.frames; ++i) {
      QImage img;
      if (!queue_.pop(img)) break;
      if (stop()) break;
      if (!convertAndEncode(img, i)) return false;
      framesDone_ = i + 1;
      if (aenc_ && !feedAudio(audio::frameToSample(p_.in + i + 1, p_.fps) - audio::frameToSample(p_.in, p_.fps), false)) return false;
      report(i + 1, QStringLiteral("rendering"));
    }
    if (stop()) return false;
    if (framesDone_ != p_.frames) {
      fail(QStringLiteral("The renderer stopped after %1 of %2 frames").arg(framesDone_.load()).arg(p_.frames));
      return false;
    }
  }
  if (aenc_) {
    // audio-only: all of it; with video: whatever is left after the last frame (rounding of the final boundary)
    const bool audioOnly = !venc_;
    while (audioRendered_ < p_.audioSamples) {
      if (!feedAudio(std::min<std::int64_t>(p_.audioSamples, audioRendered_ + kAudioChunk * (audioOnly ? 1 : 10)), false)) return false;
      if (audioOnly) {
        framesDone_ = audio::sampleToFrame(audioRendered_, p_.fps);
        report(framesDone_, QStringLiteral("rendering"));
      }
    }
    if (!feedAudio(p_.audioSamples, true)) return false;
  }
  if (stop()) return false;
  report(p_.frames, QStringLiteral("finishing"), true);
  // flush
  if (venc_) {
    avcodec_send_frame(venc_.get(), nullptr);
    if (!drain(venc_.get(), vst_)) return false;
  }
  if (aenc_) {
    avcodec_send_frame(aenc_.get(), nullptr);
    if (!drain(aenc_.get(), ast_)) return false;
  }
  return true;
}

void Job::renderLoop() {
  QString err;
  auto off = render::OffscreenRenderer::create(&err);
  if (!off) {
    fail(QStringLiteral("Can't start the renderer: %1").arg(err));
    return;
  }
  off->compositor().setDelivery(p_.delivery);

  TimelineDoc doc = doc_;
  doc.project.width = p_.canvasWidth;
  doc.project.height = p_.canvasHeight;

  FrameService::Options o;
  o.cacheBytes = 256ll << 20;
  o.gpuFrames = false;
  if (!s_.hardwareDecode) o.hw = HwMode::Off;
  FrameService service(o);
  render::FrameServiceProvider provider(service, assets_, render::FrameServiceProvider::Mode::Blocking);
  provider.openAll();

  const bool wide = p_.bits > 8;
  for (qint64 i = 0; i < p_.frames; ++i) {
    if (stop()) break;
    provider.prepare(doc, p_.in + i, 1); // read-ahead: decoders run ahead of the renderer
    render::RenderStats stats;
    QImage img = wide ? off->renderRgba64(doc, p_.in + i, provider, &stats) : off->render(doc, p_.in + i, provider, &stats);
    if (img.isNull()) {
      fail(QStringLiteral("The renderer returned no picture for frame %1").arg(p_.in + i));
      break;
    }
    if (stats.missing > 0) ++missing_;
    if (!queue_.push(std::move(img))) break;
  }
  queue_.close();
}

void Job::abandon() {
  if (fmt_ && fmt_->pb) avio_closep(&fmt_->pb);
  if (fmt_) {
    avformat_free_context(fmt_);
    fmt_ = nullptr;
  }
  if (!tmpPath_.isEmpty()) QFile::remove(tmpPath_);
}

ExportResult Job::run() {
  ExportResult res;
  const QString finalPath = QFileInfo(s_.outputPath).absoluteFilePath();
  res.path = finalPath;
  clock_.start();
  pkt_.reset(av_packet_alloc());

  const bool ok = [&] {
    if (!openOutput()) return false;
    if (p_.video != VideoCodec::None && !openVideoEncoder()) return false;
    if (p_.audio != AudioCodec::None && !openAudioEncoder()) return false;
    if (aenc_) {
      auto sources = std::make_shared<audio::FileProvider>();
      std::map<QString, audio::FileProvider::File> files;
      for (const auto& [id, ref] : assets_) files[id] = {ref.path, ref.kind != AssetKind::Image};
      sources->setFiles(std::move(files));
      if (s_.loudnessLufs && !measureLoudness()) return false;
      engine_ = std::make_unique<audio::AudioEngine>(std::make_shared<const TimelineDoc>(doc_), sources);
    }
    AVDictionary* opts = nullptr;
    if (s_.faststart && (p_.container == Container::Mp4 || p_.container == Container::Mov)) av_dict_set(&opts, "movflags", "+faststart", 0);
    const int w = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (w < 0) {
      fail(QStringLiteral("Can't write the file header: %1").arg(av_err(w)));
      return false;
    }
    headerWritten_ = true;

    // render on this thread, encode on another
    std::thread encoder([&] {
      const bool good = encodeLoop();
      if (!good) {
        queue_.close(); // unblock the renderer
      }
    });
    if (venc_) renderLoop();
    encoder.join();
    if (stop()) return false;

    const int t = av_write_trailer(fmt_);
    if (t < 0) {
      fail(QStringLiteral("Can't finish the file: %1").arg(av_err(t)));
      return false;
    }
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return true;
  }();

  res.seconds = static_cast<double>(clock_.elapsed()) / 1000.0;
  res.videoEncoder = videoEncoderName_;
  res.audioEncoder = audioEncoderName_;
  res.appliedGainDb = gainDb_;
  res.missingFrames = missing_;
  res.warnings = p_.warnings;
  if (!ok) {
    abandon();
    if (failed_) {
      res.error = error_;
    } else {
      res.cancelled = true;
    }
    return res;
  }
  // publish: replace an existing file only now that the new one is complete
  if (QFile::exists(finalPath) && !QFile::remove(finalPath)) {
    QFile::remove(tmpPath_);
    res.error = QStringLiteral("Can't replace %1 (is it open in another program?)").arg(QDir::toNativeSeparators(finalPath));
    return res;
  }
  if (!QFile::rename(tmpPath_, finalPath)) {
    QFile::remove(tmpPath_);
    res.error = QStringLiteral("Can't move the finished file to %1").arg(QDir::toNativeSeparators(finalPath));
    return res;
  }
  res.ok = true;
  res.videoFrames = venc_ ? framesDone_.load() : 0;
  res.audioSamples = aenc_ ? audioEncoded_ : 0;
  res.averageFps = res.seconds > 0 && venc_ ? static_cast<double>(res.videoFrames) / res.seconds : 0;
  return res;
}

} // namespace

ExportResult runExport(const TimelineDoc& doc, const render::AssetTable& assets, const ExportSettings& settings, const ProgressFn& progress,
                       const std::atomic<bool>* cancel) {
  ExportResult res;
  res.path = QFileInfo(settings.outputPath).absoluteFilePath();
  try {
    if (settings.outputPath.isEmpty()) {
      res.error = QStringLiteral("No output file given");
      return res;
    }
    QString err;
    const auto plan = planExport(doc, settings, &err);
    if (!plan) {
      res.error = err;
      return res;
    }
    if (plan->video != VideoCodec::None && !softwareEncoderAvailable(plan->video)) {
      res.error = QStringLiteral("This FFmpeg build has no %1 encoder").arg(plan->videoEncoder);
      return res;
    }
    if (QFileInfo::exists(res.path) && !settings.overwrite) {
      res.error = QStringLiteral("%1 already exists (overwrite was not allowed)").arg(QDir::toNativeSeparators(res.path));
      return res;
    }
    Job job(doc, assets, settings, *plan, progress, cancel);
    return job.run();
  } catch (const std::exception& e) {
    res.ok = false;
    res.error = QStringLiteral("Export failed: %1").arg(QString::fromUtf8(e.what()));
    return res;
  }
}

ExportJob::ExportJob(TimelineDoc doc, render::AssetTable assets, ExportSettings settings, QObject* parent)
    : QObject(parent), doc_(std::move(doc)), assets_(std::move(assets)), settings_(std::move(settings)) {
  qRegisterMetaType<sf::xport::ExportProgress>();
  qRegisterMetaType<sf::xport::ExportResult>();
}

ExportJob::~ExportJob() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

void ExportJob::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] {
    const ExportResult r = runExport(
        doc_, assets_, settings_, [this](const ExportProgress& p) { emit progress(p); }, &cancel_);
    running_ = false;
    emit finished(r);
  });
}

void ExportJob::cancel() { cancel_ = true; }

} // namespace sf::xport
