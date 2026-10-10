#pragma once

// What to export and how: settings, presets and the plan they resolve to for a given project.
//
// Presets follow the Electron app (apps/desktop/src/shared/export-options.ts): a quality tier
// ("720p" / "1080p" / "1440p") names the SHORT side of the picture and keeps the canvas aspect, the
// platform presets ("TikTok / Reels / Shorts", "YouTube 1080p", "Square 1080") have fixed sizes and
// bitrates. On top of those the native app has quality presets (draft / standard / high / master) that
// pick CRF + encoder preset, and professional intermediates (ProRes, DNxHR).

#include "core/schema.h"
#include "render/color.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace sf::xport {

enum class Container { Auto, Mp4, Mov, Mkv, Wav };
enum class VideoCodec { H264, H265, ProRes, DnxHr, None }; // None: audio only
enum class AudioCodec { Aac, Pcm16, Pcm24, Pcm32f, None };
enum class Hardware { Off, Auto }; // Auto: NVENC when the encoder opens on this machine, else software
enum class ProResProfile { Proxy = 0, Lt = 1, Standard = 2, Hq = 3, P4444 = 4 };

struct ExportSettings {
  QString outputPath;
  bool overwrite = false; // refuse an existing file unless set

  Container container = Container::Auto; // Auto: from the output file's extension
  VideoCodec video = VideoCodec::H264;
  AudioCodec audio = AudioCodec::Aac;    // Wav containers need a PCM codec; Auto-corrected by plan()
  Hardware hardware = Hardware::Off;

  // Output size. 0x0: the project's. A different size with the project's aspect scales the finished
  // frame (bicubic, in the colour conversion); a different aspect re-lays the project out on a canvas of
  // that size (items are fitted "contain" as always, their pixel offsets stay as they are).
  int width = 0;
  int height = 0;

  // Timeline frames [inFrame, outFrame); default the whole timeline.
  std::optional<Frame> inFrame;
  std::optional<Frame> outFrame;

  // Video quality: constant quality unless videoBitrateK > 0 (average bitrate, as the Electron presets).
  int crf = -1; // -1: the codec's default (x264 20, x265 23, NVENC cq 23)
  QString encoderPreset; // x264 / x265: ultrafast .. veryslow (default "medium"); NVENC: p1 .. p7 (default "p5")
  int videoBitrateK = 0;
  bool tenBit = false; // HEVC Main10 (H.264 10-bit where the build allows); implied by HDR output and by ProRes / DNxHR HQX
  ProResProfile proresProfile = ProResProfile::Hq;
  QString dnxProfile = QStringLiteral("dnxhr_hq"); // dnxhr_lb | dnxhr_sq | dnxhr_hq | dnxhr_hqx | dnxhr_444

  int audioBitrateK = 192;
  // Normalise the mix to this integrated loudness (LUFS: -14 streaming, -16 podcasts, -23 EBU R128) with a
  // static gain; the true peak is held under truePeakCeilingDb when that is set.
  std::optional<double> loudnessLufs;
  std::optional<double> truePeakCeilingDb;

  // Decode source video on the GPU (D3D11VA). Off by default: export should not fight other GPU work.
  bool hardwareDecode = false;
  // Mux timestamps are exact either way; this only adds `-movflags +faststart` for mp4 / mov.
  bool faststart = true;
};

struct ExportPlan {
  Container container = Container::Mp4;
  VideoCodec video = VideoCodec::H264;
  AudioCodec audio = AudioCodec::Aac;
  QString videoEncoder; // "libx264", "libx265", "prores_ks", "dnxhd", or the NVENC name when hardware is requested
  int width = 0, height = 0;           // output picture
  int canvasWidth = 0, canvasHeight = 0; // what the compositor renders (differs when the aspect changes)
  int fps = 30;
  Frame in = 0, out = 0; // [in, out)
  qint64 frames = 0;
  qint64 audioSamples = 0; // 48 kHz
  int bits = 8;            // 8 or 10 per component
  render::Delivery delivery;
  // stream tags: "bt709" / "bt2020", "bt709" / "smpte2084" / "arib-std-b67", "bt709" / "bt2020nc"
  QString primaries, transfer, matrix;
  bool hdr = false;
  QStringList warnings;
};

// Resolves settings against a project. nullopt with *error set when they can't be satisfied (unknown
// codec / container combination, empty range, odd custom size, HDR with an 8-bit codec ...).
std::optional<ExportPlan> planExport(const TimelineDoc& doc, const ExportSettings& settings, QString* error = nullptr);

// ---- presets ----
struct Preset {
  QString name;
  int width = 0;  // 0: tier, see shortSide
  int height = 0;
  int shortSide = 0;
  int videoBitrateK = 0; // 0: constant quality
  VideoCodec video = VideoCodec::H264;
  Container container = Container::Mp4;
};
// "TikTok / Reels / Shorts", "Vertical", "YouTube 1080p", "Square 1080", "1440p", "720p", "1080p", "2160p".
std::vector<Preset> listPresets();
// Case-insensitive; fills width/height (tiers relative to the canvas), bitrate, codec and container.
// False when the name is unknown.
bool applyPreset(ExportSettings& settings, const QString& name, int canvasWidth, int canvasHeight);
// "draft" | "standard" | "high" | "master": CRF and encoder preset for the settings' codec.
bool applyQuality(ExportSettings& settings, const QString& quality);

// Electron's resolveExport: the short side of the canvas becomes `shortSide`, both sides even.
void resolveTierSize(int shortSide, int canvasWidth, int canvasHeight, int* width, int* height);

QString containerName(Container c);
QString videoCodecName(VideoCodec c);
std::optional<Container> containerFromName(const QString& name);
std::optional<VideoCodec> videoCodecFromName(const QString& name);
std::optional<AudioCodec> audioCodecFromName(const QString& name);

// Whether this FFmpeg build has the software encoder for the codec (libx264 / libx265 / prores_ks / dnxhd).
bool softwareEncoderAvailable(VideoCodec codec);
// Opens a tiny NVENC session to see whether it works on this machine. Touches the GPU: only called
// when the user asked for hardware. Cached after the first call.
bool hardwareEncoderUsable(VideoCodec codec);

} // namespace sf::xport
