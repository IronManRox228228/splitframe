#include "export/export_settings.h"

#include "audio/timebase.h"
#include "core/timeline_doc.h"
#include "media/ffmpeg.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace sf::xport {

namespace {

int even(double n) { return std::max(2, static_cast<int>(std::lround(n / 2.0)) * 2); }

QString lower(const QString& s) { return s.trimmed().toLower(); }

bool isPcm(AudioCodec a) { return a == AudioCodec::Pcm16 || a == AudioCodec::Pcm24 || a == AudioCodec::Pcm32f; }

const char* softwareName(VideoCodec c) {
  switch (c) {
    case VideoCodec::H264: return "libx264";
    case VideoCodec::H265: return "libx265";
    case VideoCodec::ProRes: return "prores_ks";
    case VideoCodec::DnxHr: return "dnxhd";
    case VideoCodec::None: break;
  }
  return "";
}

} // namespace

QString containerName(Container c) {
  switch (c) {
    case Container::Auto: return QStringLiteral("auto");
    case Container::Mp4: return QStringLiteral("mp4");
    case Container::Mov: return QStringLiteral("mov");
    case Container::Mkv: return QStringLiteral("mkv");
    case Container::Wav: return QStringLiteral("wav");
  }
  return {};
}

QString videoCodecName(VideoCodec c) {
  switch (c) {
    case VideoCodec::H264: return QStringLiteral("h264");
    case VideoCodec::H265: return QStringLiteral("h265");
    case VideoCodec::ProRes: return QStringLiteral("prores");
    case VideoCodec::DnxHr: return QStringLiteral("dnxhr");
    case VideoCodec::None: return QStringLiteral("none");
  }
  return {};
}

std::optional<Container> containerFromName(const QString& name) {
  const QString n = lower(name);
  if (n == QLatin1String("auto")) return Container::Auto;
  if (n == QLatin1String("mp4") || n == QLatin1String("m4v")) return Container::Mp4;
  if (n == QLatin1String("mov")) return Container::Mov;
  if (n == QLatin1String("mkv") || n == QLatin1String("matroska")) return Container::Mkv;
  if (n == QLatin1String("wav")) return Container::Wav;
  return std::nullopt;
}

std::optional<VideoCodec> videoCodecFromName(const QString& name) {
  const QString n = lower(name);
  if (n == QLatin1String("h264") || n == QLatin1String("x264") || n == QLatin1String("avc")) return VideoCodec::H264;
  if (n == QLatin1String("h265") || n == QLatin1String("hevc") || n == QLatin1String("x265")) return VideoCodec::H265;
  if (n == QLatin1String("prores")) return VideoCodec::ProRes;
  if (n == QLatin1String("dnxhr") || n == QLatin1String("dnxhd")) return VideoCodec::DnxHr;
  if (n == QLatin1String("none") || n == QLatin1String("audio")) return VideoCodec::None;
  return std::nullopt;
}

std::optional<AudioCodec> audioCodecFromName(const QString& name) {
  const QString n = lower(name);
  if (n == QLatin1String("aac")) return AudioCodec::Aac;
  if (n == QLatin1String("pcm16") || n == QLatin1String("pcm") || n == QLatin1String("pcm_s16le")) return AudioCodec::Pcm16;
  if (n == QLatin1String("pcm24") || n == QLatin1String("pcm_s24le")) return AudioCodec::Pcm24;
  if (n == QLatin1String("pcm32f") || n == QLatin1String("pcmf32") || n == QLatin1String("float")) return AudioCodec::Pcm32f;
  if (n == QLatin1String("none")) return AudioCodec::None;
  return std::nullopt;
}

void resolveTierSize(int shortSide, int canvasWidth, int canvasHeight, int* width, int* height) {
  const double scale = static_cast<double>(shortSide) / std::max(1, std::min(canvasWidth, canvasHeight));
  *width = even(canvasWidth * scale);
  *height = even(canvasHeight * scale);
}

std::vector<Preset> listPresets() {
  std::vector<Preset> p;
  p.push_back({QStringLiteral("TikTok / Reels / Shorts"), 1080, 1920, 0, 12000, VideoCodec::H264, Container::Mp4});
  p.push_back({QStringLiteral("Vertical"), 1080, 1920, 0, 12000, VideoCodec::H264, Container::Mp4});
  p.push_back({QStringLiteral("YouTube 1080p"), 1920, 1080, 0, 12000, VideoCodec::H264, Container::Mp4});
  p.push_back({QStringLiteral("Square 1080"), 1080, 1080, 0, 10000, VideoCodec::H264, Container::Mp4});
  p.push_back({QStringLiteral("1440p"), 2560, 1440, 0, 20000, VideoCodec::H264, Container::Mp4});
  for (const int side : {720, 1080, 1440, 2160}) p.push_back({QStringLiteral("%1p").arg(side), 0, 0, side, 0, VideoCodec::H264, Container::Mp4});
  return p;
}

bool applyPreset(ExportSettings& s, const QString& name, int canvasWidth, int canvasHeight) {
  QString key = lower(name);
  if (key.endsWith(QLatin1String(" mp4"))) key.chop(4);
  const auto presets = listPresets();
  // a bare "1440p" is the fixed platform preset (2560x1440 at 20 Mbps): the first match wins, as in Electron's findPreset
  for (const Preset& p : presets) {
    if (p.name.toLower() != key) continue;
    if (p.shortSide > 0) {
      resolveTierSize(p.shortSide, canvasWidth, canvasHeight, &s.width, &s.height);
      // Electron: bitrate scales with pixel count, base 12 Mbps at 1080p, never below 1.5 Mbps
      const double px = static_cast<double>(s.width) * s.height;
      s.videoBitrateK = std::max(1500, static_cast<int>(std::lround(12000.0 * px / (1920.0 * 1080.0) / 100.0)) * 100);
    } else {
      s.width = p.width;
      s.height = p.height;
      s.videoBitrateK = p.videoBitrateK;
    }
    return true;
  }
  return false;
}

bool applyQuality(ExportSettings& s, const QString& quality) {
  const QString q = lower(quality);
  int level = -1;
  if (q == QLatin1String("draft")) level = 0;
  else if (q == QLatin1String("standard")) level = 1;
  else if (q == QLatin1String("high")) level = 2;
  else if (q == QLatin1String("master")) level = 3;
  if (level < 0) return false;
  static const int crf264[] = {28, 21, 18, 14};
  static const char* const preset[] = {"veryfast", "medium", "slow", "slow"};
  static const char* const nv[] = {"p3", "p5", "p6", "p7"};
  static const ProResProfile prores[] = {ProResProfile::Lt, ProResProfile::Standard, ProResProfile::Hq, ProResProfile::Hq};
  static const char* const dnx[] = {"dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx"};
  s.videoBitrateK = 0;
  s.crf = crf264[level] + (s.video == VideoCodec::H265 ? 3 : 0);
  s.encoderPreset = QString::fromLatin1(s.hardware == Hardware::Auto ? nv[level] : preset[level]);
  s.proresProfile = prores[level];
  s.dnxProfile = QString::fromLatin1(dnx[level]);
  return true;
}

bool softwareEncoderAvailable(VideoCodec codec) {
  const char* n = softwareName(codec);
  return *n && avcodec_find_encoder_by_name(n) != nullptr;
}

bool hardwareEncoderUsable(VideoCodec codec) {
  if (codec != VideoCodec::H264 && codec != VideoCodec::H265) return false;
  static std::mutex m;
  static int cached[2] = {-1, -1};
  const std::lock_guard lock(m);
  int& slot = cached[codec == VideoCodec::H264 ? 0 : 1];
  if (slot >= 0) return slot == 1;
  slot = 0;
  const AVCodec* enc = avcodec_find_encoder_by_name(codec == VideoCodec::H264 ? "h264_nvenc" : "hevc_nvenc");
  if (!enc) return false;
  av::CodecPtr ctx(avcodec_alloc_context3(enc));
  ctx->width = 256;
  ctx->height = 256;
  ctx->time_base = {1, 30};
  ctx->framerate = {30, 1};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  slot = avcodec_open2(ctx.get(), enc, nullptr) >= 0 ? 1 : 0;
  return slot == 1;
}

std::optional<ExportPlan> planExport(const TimelineDoc& doc, const ExportSettings& s, QString* error) {
  auto fail = [&](const QString& msg) -> std::optional<ExportPlan> {
    if (error) *error = msg;
    return std::nullopt;
  };
  ExportPlan p;
  p.fps = static_cast<int>(doc.project.fps);
  if (p.fps < 1) return fail(QStringLiteral("The project frame rate is invalid"));

  const Frame duration = docDurationFrames(doc);
  p.in = s.inFrame.value_or(0);
  p.out = s.outFrame.value_or(duration);
  if (p.in < 0) return fail(QStringLiteral("The export range starts before frame 0"));
  if (p.out <= p.in) return fail(QStringLiteral("Nothing to export: the range [%1, %2) is empty").arg(p.in).arg(p.out));
  p.frames = p.out - p.in;
  p.audioSamples = audio::frameToSample(p.out, p.fps) - audio::frameToSample(p.in, p.fps);

  // container
  p.container = s.container;
  if (p.container == Container::Auto) {
    const QString ext = QFileInfo(s.outputPath).suffix().toLower();
    const auto c = containerFromName(ext);
    if (!c || *c == Container::Auto) return fail(QStringLiteral("Can't tell the container from the file name \"%1\" (use .mp4, .mov, .mkv or .wav)").arg(s.outputPath));
    p.container = *c;
  }
  p.video = s.video;
  p.audio = s.audio;
  if (p.container == Container::Wav) {
    p.video = VideoCodec::None;
    if (!isPcm(p.audio)) p.audio = AudioCodec::Pcm16;
  }
  if (p.video == VideoCodec::None && p.audio == AudioCodec::None) return fail(QStringLiteral("Nothing to export: no video and no audio"));
  if (isPcm(p.audio) && p.container == Container::Mp4) return fail(QStringLiteral("PCM audio does not belong in an mp4: use .mov, .mkv or .wav, or AAC"));
  if (p.container == Container::Mp4 && (p.video == VideoCodec::ProRes || p.video == VideoCodec::DnxHr))
    return fail(QStringLiteral("%1 does not belong in an mp4: use .mov or .mkv").arg(videoCodecName(p.video)));

  // picture
  const int pw = static_cast<int>(doc.project.width), ph = static_cast<int>(doc.project.height);
  p.canvasWidth = pw;
  p.canvasHeight = ph;
  p.width = pw;
  p.height = ph;
  if (p.video != VideoCodec::None) {
    if (s.width > 0 || s.height > 0) {
      if (s.width <= 0 || s.height <= 0) return fail(QStringLiteral("Set both width and height, or neither"));
      p.width = s.width;
      p.height = s.height;
    }
    if (p.width % 2 || p.height % 2) return fail(QStringLiteral("The output size %1x%2 must be even in both directions (chroma subsampling)").arg(p.width).arg(p.height));
    if (p.width < 16 || p.height < 16) return fail(QStringLiteral("The output size %1x%2 is too small").arg(p.width).arg(p.height));
    const double aspectOut = static_cast<double>(p.width) / p.height, aspectIn = static_cast<double>(pw) / ph;
    if (std::abs(aspectOut - aspectIn) / aspectIn > 0.005) {
      p.canvasWidth = p.width;
      p.canvasHeight = p.height;
    }
  }

  // colour: the project's delivery space decides the tags
  const QString space = doc.project.colorManagement ? doc.project.colorManagement->outputSpace.value_or(QString()) : QString();
  QString useSpace = space;
  const render::Delivery probe = render::deliveryFromSpace(space, 8);
  p.hdr = render::isHdrDelivery(probe);
  if (!probe.ocioSpace.isEmpty()) {
    p.warnings << QStringLiteral("Output space \"%1\" needs an OpenColorIO config that the exporter does not load; delivering Rec.709").arg(space);
    useSpace = QStringLiteral("rec709");
  }

  p.bits = 8;
  if (s.tenBit || p.hdr) p.bits = 10;
  if (p.video == VideoCodec::ProRes) p.bits = 10;
  if (p.video == VideoCodec::DnxHr && (s.dnxProfile == QLatin1String("dnxhr_hqx") || s.dnxProfile == QLatin1String("dnxhr_444"))) p.bits = 10;
  if (p.video == VideoCodec::None) p.bits = 8;
  if (p.hdr && p.video == VideoCodec::H264) return fail(QStringLiteral("HDR (%1) needs 10-bit video: choose HEVC, ProRes or DNxHR").arg(space));
  if (p.hdr && p.video == VideoCodec::DnxHr && p.bits < 10) return fail(QStringLiteral("HDR needs a 10-bit DNxHR profile (dnxhr_hqx or dnxhr_444)"));
  if (p.video == VideoCodec::DnxHr) {
    static const QStringList profiles{QStringLiteral("dnxhr_lb"), QStringLiteral("dnxhr_sq"), QStringLiteral("dnxhr_hq"), QStringLiteral("dnxhr_hqx"), QStringLiteral("dnxhr_444")};
    if (!profiles.contains(s.dnxProfile)) return fail(QStringLiteral("Unknown DNxHR profile \"%1\"").arg(s.dnxProfile));
  }
  p.delivery = render::deliveryFromSpace(useSpace, p.bits);
  if (p.hdr) {
    p.primaries = QStringLiteral("bt2020");
    p.matrix = QStringLiteral("bt2020nc");
    p.transfer = p.delivery.encoding == render::Delivery::Encoding::Rec2100Pq ? QStringLiteral("smpte2084") : QStringLiteral("arib-std-b67");
  } else {
    // sRGB-encoded and Rec.709 deliveries are both tagged BT.709: that is what the rest of the pipeline
    // (and every player) takes untagged / BT.709 video for (see render/color.h, transferFor).
    p.primaries = QStringLiteral("bt709");
    p.matrix = QStringLiteral("bt709");
    p.transfer = QStringLiteral("bt709");
  }
  p.videoEncoder = QString::fromLatin1(softwareName(p.video));
  return p;
}

} // namespace sf::xport
