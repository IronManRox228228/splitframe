#include "media/audio_decoder.h"

#include "media/ffmpeg.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sf {

namespace {
constexpr int kRate = AudioDecoder::kSampleRate;
constexpr int kCh = AudioDecoder::kChannels;
constexpr qint64 kPreroll = 4800;       // 100 ms of decoder warm-up before the wanted sample
constexpr qint64 kKeepBehind = 24000;   // tail kept after a read so slightly overlapping reads don't re-seek
constexpr qint64 kForwardSlack = 96000; // decode-and-discard up to 2 s instead of seeking
} // namespace

struct AudioDecoder::Impl {
  QString path;
  av::FormatPtr fmt;
  int stream = -1;
  AVStream* st = nullptr;
  av::CodecPtr ctx;
  av::SwrPtr swr;
  av::PacketPtr pkt{av_packet_alloc()};
  av::FramePtr frame{av_frame_alloc()};
  AudioStreamInfo info;
  int64_t startPts = 0; // stream timestamp of output sample 0

  std::vector<float> pending; // interleaved stereo produced by the resampler
  qint64 pendingStart = 0;    // output sample index of pending[0]
  qint64 decodePos = 0;       // output sample index just past pending
  bool positioned = false;
  bool needAlign = false; // next decoded frame defines pendingStart (after a seek)
  bool draining = false;
  bool eof = false;
  std::vector<float> scratch;

  bool initResampler();
  bool seek(qint64 sample, qint64 preroll);
  bool decodeMore();
  void append(const AVFrame* f);
  void flushResampler();
  qint64 pendingFrames() const { return static_cast<qint64>(pending.size()) / kCh; }
};

bool AudioDecoder::Impl::initResampler() {
  AVChannelLayout out{};
  av_channel_layout_default(&out, kCh);
  AVChannelLayout in{};
  av_channel_layout_copy(&in, &ctx->ch_layout);
  if (in.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, in.nb_channels > 0 ? in.nb_channels : 2);

  SwrContext* raw = nullptr;
  const int rc = swr_alloc_set_opts2(&raw, &out, AV_SAMPLE_FMT_FLT, kRate, &in, ctx->sample_fmt, ctx->sample_rate, 0, nullptr);
  swr.reset(raw);
  if (rc < 0 || !swr) return false;
  if (in.nb_channels == 1) {
    // swresample's default mono -> stereo is -3 dB per side; an editor wants the same level on both
    const double m[2] = {1.0, 1.0};
    swr_set_matrix(swr.get(), m, 1);
  }
  av_channel_layout_uninit(&in);
  av_channel_layout_uninit(&out);
  return swr_init(swr.get()) >= 0;
}

bool AudioDecoder::Impl::seek(qint64 sample, qint64 preroll) {
  const qint64 from = std::max<qint64>(0, sample - preroll);
  const int64_t ts = startPts + av_rescale_q(from, AVRational{1, kRate}, st->time_base);
  int rc = av_seek_frame(fmt.get(), stream, ts, AVSEEK_FLAG_BACKWARD);
  if (rc < 0 && from == 0) rc = av_seek_frame(fmt.get(), -1, fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0, AVSEEK_FLAG_BACKWARD);
  if (rc < 0) return false;
  avcodec_flush_buffers(ctx.get());
  if (!initResampler()) return false;
  pending.clear();
  pendingStart = decodePos = sample;
  positioned = true;
  needAlign = true;
  draining = eof = false;
  return true;
}

void AudioDecoder::Impl::append(const AVFrame* f) {
  if (needAlign) {
    const int64_t pts = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
    if (pts != AV_NOPTS_VALUE) pendingStart = decodePos = av_rescale_q(pts - startPts, st->time_base, AVRational{1, kRate});
    needAlign = false;
    pending.clear();
  }
  const int cap = static_cast<int>(av_rescale_rnd(swr_get_delay(swr.get(), ctx->sample_rate) + f->nb_samples, kRate, ctx->sample_rate, AV_ROUND_UP)) + 64;
  const size_t old = pending.size();
  pending.resize(old + static_cast<size_t>(cap) * kCh);
  uint8_t* outp = reinterpret_cast<uint8_t*>(pending.data() + old);
  const int n = swr_convert(swr.get(), &outp, cap, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
  pending.resize(old + static_cast<size_t>(std::max(n, 0)) * kCh);
  if (n > 0) decodePos += n;
}

void AudioDecoder::Impl::flushResampler() {
  const int cap = static_cast<int>(swr_get_delay(swr.get(), kRate)) + 64;
  const size_t old = pending.size();
  pending.resize(old + static_cast<size_t>(cap) * kCh);
  uint8_t* outp = reinterpret_cast<uint8_t*>(pending.data() + old);
  const int n = swr_convert(swr.get(), &outp, cap, nullptr, 0);
  pending.resize(old + static_cast<size_t>(std::max(n, 0)) * kCh);
  if (n > 0) decodePos += n;
}

// Produces at least one more chunk of output, or reports the end of the stream.
bool AudioDecoder::Impl::decodeMore() {
  while (true) {
    const int rc = avcodec_receive_frame(ctx.get(), frame.get());
    if (rc == 0) {
      append(frame.get());
      av_frame_unref(frame.get());
      return true;
    }
    if (rc == AVERROR(EAGAIN)) {
      const int r = av_read_frame(fmt.get(), pkt.get());
      if (r < 0) {
        draining = true;
        avcodec_send_packet(ctx.get(), nullptr);
      } else {
        if (pkt->stream_index == stream) avcodec_send_packet(ctx.get(), pkt.get());
        av_packet_unref(pkt.get());
      }
      continue;
    }
    if (rc == AVERROR_EOF) {
      const qint64 before = decodePos;
      flushResampler();
      eof = true;
      return decodePos > before;
    }
    if (rc == AVERROR_INVALIDDATA) continue;
    eof = true;
    return false;
  }
}

AudioDecoder::AudioDecoder() : d(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

std::unique_ptr<AudioDecoder> AudioDecoder::open(const QString& path, QString* error) {
  std::unique_ptr<AudioDecoder> dec(new AudioDecoder());
  Impl& d = *dec->d;
  auto fail = [&](const QString& msg) -> std::unique_ptr<AudioDecoder> {
    if (error) *error = msg;
    return nullptr;
  };
  QString err;
  d.path = path;
  d.fmt = av::openInput(path, &err);
  if (!d.fmt) return fail(err);
  const AVCodec* codec = nullptr;
  const int a = av_find_best_stream(d.fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (a < 0 || !codec) return fail(QStringLiteral("%1 has no decodable audio stream").arg(path));
  d.stream = a;
  d.st = d.fmt->streams[a];
  for (unsigned i = 0; i < d.fmt->nb_streams; ++i) {
    if (static_cast<int>(i) != a) d.fmt->streams[i]->discard = AVDISCARD_ALL;
  }
  d.ctx.reset(avcodec_alloc_context3(codec));
  if (!d.ctx || avcodec_parameters_to_context(d.ctx.get(), d.st->codecpar) < 0) return fail(QStringLiteral("Can't set up audio decoder"));
  d.ctx->pkt_timebase = d.st->time_base;
  if (const int rc = avcodec_open2(d.ctx.get(), codec, nullptr); rc < 0) return fail(QStringLiteral("Can't open audio decoder: %1").arg(av::errorString(rc)));
  if (!d.initResampler()) return fail(QStringLiteral("Can't set up resampler for %1").arg(path));

  d.startPts = d.st->start_time != AV_NOPTS_VALUE ? d.st->start_time : 0;
  AudioStreamInfo& info = d.info;
  info.sourceRate = d.ctx->sample_rate;
  info.sourceChannels = d.ctx->ch_layout.nb_channels;
  info.codec = QString::fromUtf8(avcodec_get_name(d.st->codecpar->codec_id));
  const double tb = av_q2d(d.st->time_base);
  if (d.st->duration != AV_NOPTS_VALUE && d.st->duration > 0) info.durationSec = d.st->duration * tb;
  else if (d.fmt->duration > 0) info.durationSec = d.fmt->duration / double(AV_TIME_BASE);
  info.totalSamples = static_cast<qint64>(std::llround(info.durationSec * kRate));
  const double base = d.fmt->start_time != AV_NOPTS_VALUE ? d.fmt->start_time / double(AV_TIME_BASE) : 0.0;
  info.startSec = d.startPts * tb - base;
  return dec;
}

const AudioStreamInfo& AudioDecoder::info() const { return d->info; }

qint64 AudioDecoder::read(qint64 start, qint64 count, float* out) {
  Impl& s = *d;
  if (count <= 0) return 0;
  std::memset(out, 0, static_cast<size_t>(count) * kCh * sizeof(float));
  const qint64 lo = std::max<qint64>(start, 0);
  const qint64 hi = start + count;
  if (lo >= hi) return 0;

  const bool continues = s.positioned && !s.needAlign && lo >= s.pendingStart && lo <= s.decodePos + kForwardSlack;
  if (!continues) {
    // Try seeking a little earlier each time if the demuxer lands after the wanted sample
    // (some containers only seek to coarse points).
    for (int attempt = 0; attempt < 4; ++attempt) {
      if (!s.seek(lo, kPreroll << (attempt * 2))) return 0;
      if (!s.decodeMore()) break;
      if (s.pendingStart <= lo || lo == 0) break;
    }
  }
  while (s.decodePos < hi && !s.eof) {
    if (!s.decodeMore() && s.eof) break;
  }

  const qint64 from = std::max(lo, s.pendingStart);
  const qint64 to = std::min(hi, s.decodePos);
  qint64 produced = 0;
  if (to > from) {
    produced = to - from;
    std::memcpy(out + (from - start) * kCh, s.pending.data() + (from - s.pendingStart) * kCh,
                static_cast<size_t>(produced) * kCh * sizeof(float));
  }
  // drop what is safely behind us
  const qint64 drop = std::min(s.pendingFrames(), std::max<qint64>(0, hi - kKeepBehind - s.pendingStart));
  if (drop > 0) {
    s.pending.erase(s.pending.begin(), s.pending.begin() + static_cast<std::ptrdiff_t>(drop * kCh));
    s.pendingStart += drop;
  }
  return produced;
}

std::vector<float> AudioDecoder::read(qint64 start, qint64 count) {
  std::vector<float> out(static_cast<size_t>(std::max<qint64>(count, 0)) * kCh);
  if (count > 0) read(start, count, out.data());
  return out;
}

} // namespace sf
