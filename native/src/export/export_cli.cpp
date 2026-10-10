#include "export/export_cli.h"

#include "export/exporter.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sf::xport {

namespace {

// splitframe.exe is a GUI-subsystem program: attach to the parent console when stdout isn't redirected.
void writeOut(const QString& text) {
  const QByteArray bytes = text.toUtf8() + '\n';
#ifdef Q_OS_WIN
  static HANDLE h = [] {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
      if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return out;
  }();
  if (h && h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    return;
  }
#endif
  std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
  std::fflush(stdout);
}

QString number(double v, int digits = 1) { return QString::number(v, 'f', digits); }

} // namespace

bool loadProjectForExport(const QString& path, TimelineDoc* doc, render::AssetTable* assets, QString* error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("Can't read %1: %2").arg(path, f.errorString());
    return false;
  }
  const QByteArray json = f.readAll();
  try {
    const QJsonDocument parsed = QJsonDocument::fromJson(json);
    if (!parsed.isObject()) throw std::runtime_error("not a project file (expected a JSON object)");
    ProjectBundle bundle;
    if (parsed.object().contains(QStringLiteral("doc"))) {
      bundle = parseProjectBundleJson(json);
    } else {
      bundle.doc = parseTimelineDocJson(json);
    }
    const QDir base = QFileInfo(path).absoluteDir();
    assets->clear();
    for (const Asset& a : bundle.assets) {
      if (a.status == AssetStatus::Failed || a.path.isEmpty()) continue;
      const QString p = QFileInfo(a.path).isAbsolute() ? a.path : QDir::cleanPath(base.absoluteFilePath(a.path));
      (*assets)[a.id] = {p, a.kind, {}};
    }
    *doc = std::move(bundle.doc);
    return true;
  } catch (const std::exception& e) {
    if (error) *error = QStringLiteral("%1: %2").arg(path, QString::fromUtf8(e.what()));
    return false;
  }
}

int runExportCli(const QStringList& args) {
  QCommandLineParser cli;
  cli.setApplicationDescription(QStringLiteral("SplitFrame headless export"));
  cli.addHelpOption();
  auto opt = [&](const char* name, const char* desc, const char* value = nullptr) {
    QCommandLineOption o(QString::fromLatin1(name), QString::fromLatin1(desc), value ? QString::fromLatin1(value) : QString());
    cli.addOption(o);
    return o;
  };
  const auto exportOpt = opt("export", "Project .json to export.", "project");
  const auto outOpt = opt("out", "Output file (.mp4 .mov .mkv .wav).", "file");
  const auto presetOpt = opt("preset", "Size preset: 720p, 1080p, 1440p, 2160p, vertical, \"TikTok / Reels / Shorts\", \"YouTube 1080p\", \"Square 1080\".", "name");
  const auto qualityOpt = opt("quality", "draft | standard | high | master.", "level");
  const auto codecOpt = opt("codec", "h264 | h265 | prores | dnxhr.", "codec");
  const auto containerOpt = opt("container", "mp4 | mov | mkv | wav (default: from the file name).", "name");
  const auto audioCodecOpt = opt("audio-codec", "aac | pcm16 | pcm24 | pcm32f | none.", "codec");
  const auto noAudioOpt = opt("no-audio", "Leave the audio out.");
  const auto audioOnlyOpt = opt("audio-only", "Export the audio only.");
  const auto crfOpt = opt("crf", "Constant quality (x264 / x265 / NVENC cq).", "n");
  const auto encPresetOpt = opt("encoder-preset", "x264 / x265 preset (medium) or NVENC p1..p7.", "preset");
  const auto bitrateOpt = opt("bitrate", "Average video bitrate in kbps instead of constant quality.", "kbps");
  const auto widthOpt = opt("width", "Output width in pixels.", "px");
  const auto heightOpt = opt("height", "Output height in pixels.", "px");
  const auto rangeOpt = opt("range", "Timeline frames to export, \"in:out\" (out exclusive).", "in:out");
  const auto tenBitOpt = opt("ten-bit", "10-bit video (HEVC Main10).");
  const auto proresOpt = opt("prores-profile", "proxy | lt | standard | hq | 4444.", "profile");
  const auto dnxOpt = opt("dnx-profile", "dnxhr_lb | dnxhr_sq | dnxhr_hq | dnxhr_hqx | dnxhr_444.", "profile");
  const auto lufsOpt = opt("lufs", "Normalise the audio to this integrated loudness.", "lufs");
  const auto peakOpt = opt("true-peak", "True-peak ceiling in dBTP for the normalisation.", "db");
  const auto overwriteOpt = opt("overwrite", "Replace an existing output file.");
  const auto hwOpt = opt("hw", "Use NVENC when it is available.");
  const auto hwDecodeOpt = opt("hw-decode", "Decode source video on the GPU.");
  const auto listOpt = opt("list-presets", "Print the size presets and exit.");
  cli.addOption(QCommandLineOption(QStringLiteral("sw"), QStringLiteral("Ignored (the exporter decodes in software unless --hw-decode).")));
  cli.addOption(QCommandLineOption(QStringLiteral("cpu"), QStringLiteral("Ignored.")));
  cli.parse(args);

  if (cli.isSet(listOpt)) {
    for (const Preset& p : listPresets()) writeOut(p.shortSide > 0 ? QStringLiteral("%1 (short side %2)").arg(p.name).arg(p.shortSide) : QStringLiteral("%1 (%2x%3, %4 kbps)").arg(p.name).arg(p.width).arg(p.height).arg(p.videoBitrateK));
    return 0;
  }
  auto usage = [&](const QString& msg) {
    writeOut(QStringLiteral("error: %1").arg(msg));
    return 2;
  };
  if (!cli.unknownOptionNames().isEmpty()) return usage(QStringLiteral("unknown option --%1").arg(cli.unknownOptionNames().first()));
  if (cli.isSet(QStringLiteral("help"))) {
    writeOut(cli.helpText());
    return 0;
  }
  const QString projectPath = cli.value(exportOpt);
  if (projectPath.isEmpty()) return usage(QStringLiteral("--export needs a project file"));
  if (!cli.isSet(outOpt)) return usage(QStringLiteral("--out <file> is required"));

  TimelineDoc doc;
  render::AssetTable assets;
  QString err;
  if (!loadProjectForExport(projectPath, &doc, &assets, &err)) return usage(err);

  ExportSettings s;
  s.outputPath = cli.value(outOpt);
  s.overwrite = cli.isSet(overwriteOpt);
  bool ok = true;
  auto intOf = [&](const QCommandLineOption& o, int* out) {
    if (!cli.isSet(o)) return;
    bool good = false;
    const int v = cli.value(o).toInt(&good);
    if (!good) ok = false;
    else *out = v;
  };
  if (cli.isSet(codecOpt)) {
    const auto c = videoCodecFromName(cli.value(codecOpt));
    if (!c) return usage(QStringLiteral("unknown codec \"%1\"").arg(cli.value(codecOpt)));
    s.video = *c;
  }
  if (cli.isSet(containerOpt)) {
    const auto c = containerFromName(cli.value(containerOpt));
    if (!c) return usage(QStringLiteral("unknown container \"%1\"").arg(cli.value(containerOpt)));
    s.container = *c;
  }
  if (cli.isSet(audioCodecOpt)) {
    const auto c = audioCodecFromName(cli.value(audioCodecOpt));
    if (!c) return usage(QStringLiteral("unknown audio codec \"%1\"").arg(cli.value(audioCodecOpt)));
    s.audio = *c;
  }
  if (cli.isSet(noAudioOpt)) s.audio = AudioCodec::None;
  if (cli.isSet(audioOnlyOpt)) s.video = VideoCodec::None;
  if (cli.isSet(hwOpt)) s.hardware = Hardware::Auto;
  s.hardwareDecode = cli.isSet(hwDecodeOpt);
  s.tenBit = cli.isSet(tenBitOpt);
  // preset and quality first so explicit options override them
  if (cli.isSet(presetOpt)) {
    QString name = cli.value(presetOpt);
    if (name.compare(QLatin1String("vertical"), Qt::CaseInsensitive) == 0) name = QStringLiteral("Vertical");
    if (!applyPreset(s, name, static_cast<int>(doc.project.width), static_cast<int>(doc.project.height))) return usage(QStringLiteral("unknown preset \"%1\" (see --list-presets)").arg(name));
  }
  if (cli.isSet(qualityOpt) && !applyQuality(s, cli.value(qualityOpt))) return usage(QStringLiteral("unknown quality \"%1\"").arg(cli.value(qualityOpt)));
  intOf(crfOpt, &s.crf);
  intOf(bitrateOpt, &s.videoBitrateK);
  intOf(widthOpt, &s.width);
  intOf(heightOpt, &s.height);
  if (!ok) return usage(QStringLiteral("a numeric option is not a number"));
  if (cli.isSet(encPresetOpt)) s.encoderPreset = cli.value(encPresetOpt);
  if (cli.isSet(proresOpt)) {
    const QString v = cli.value(proresOpt).toLower();
    if (v == QLatin1String("proxy")) s.proresProfile = ProResProfile::Proxy;
    else if (v == QLatin1String("lt")) s.proresProfile = ProResProfile::Lt;
    else if (v == QLatin1String("standard")) s.proresProfile = ProResProfile::Standard;
    else if (v == QLatin1String("hq")) s.proresProfile = ProResProfile::Hq;
    else if (v == QLatin1String("4444")) s.proresProfile = ProResProfile::P4444;
    else return usage(QStringLiteral("unknown ProRes profile \"%1\"").arg(v));
  }
  if (cli.isSet(dnxOpt)) {
    QString v = cli.value(dnxOpt).toLower();
    if (!v.startsWith(QLatin1String("dnxhr_"))) v = QStringLiteral("dnxhr_") + v;
    s.dnxProfile = v;
  }
  if (cli.isSet(lufsOpt)) {
    bool good = false;
    const double v = cli.value(lufsOpt).toDouble(&good);
    if (!good) return usage(QStringLiteral("--lufs needs a number"));
    s.loudnessLufs = v;
  }
  if (cli.isSet(peakOpt)) {
    bool good = false;
    const double v = cli.value(peakOpt).toDouble(&good);
    if (!good) return usage(QStringLiteral("--true-peak needs a number"));
    s.truePeakCeilingDb = v;
  }
  if (cli.isSet(rangeOpt)) {
    const QStringList parts = cli.value(rangeOpt).split(QLatin1Char(':'));
    bool a = false, b = false;
    const qint64 in = parts.value(0).toLongLong(&a), out = parts.value(1).toLongLong(&b);
    if (parts.size() != 2 || !a || !b) return usage(QStringLiteral("--range needs \"in:out\" in frames"));
    s.inFrame = in;
    s.outFrame = out;
  }

  const auto plan = planExport(doc, s, &err);
  if (!plan) return usage(err);
  writeOut(QStringLiteral("plan container=%1 video=%2 size=%3x%4 fps=%5 frames=%6 bits=%7 audio_samples=%8")
               .arg(containerName(plan->container), plan->video == VideoCodec::None ? QStringLiteral("none") : plan->videoEncoder)
               .arg(plan->width).arg(plan->height).arg(plan->fps).arg(plan->frames).arg(plan->bits).arg(plan->audioSamples));

  const ExportResult r = runExport(doc, assets, s, [](const ExportProgress& p) {
    writeOut(QStringLiteral("progress frame=%1 total=%2 fps=%3 eta=%4 stage=%5").arg(p.frame).arg(p.totalFrames).arg(number(p.fps)).arg(number(p.etaSec)).arg(QString(p.stage).replace(QLatin1Char(' '), QLatin1Char('_'))));
  });
  for (const QString& w : r.warnings) writeOut(QStringLiteral("warning: %1").arg(w));
  if (r.missingFrames > 0) writeOut(QStringLiteral("warning: %1 frames drew a \"media unavailable\" layer").arg(r.missingFrames));
  if (r.cancelled) {
    writeOut(QStringLiteral("cancelled"));
    return 3;
  }
  if (!r.ok) {
    writeOut(QStringLiteral("error: %1").arg(r.error));
    return 1;
  }
  writeOut(QStringLiteral("done path=%1 frames=%2 audio_samples=%3 seconds=%4 fps=%5 encoder=%6 gain_db=%7")
               .arg(QDir::toNativeSeparators(r.path)).arg(r.videoFrames).arg(r.audioSamples).arg(number(r.seconds, 2)).arg(number(r.averageFps))
               .arg(r.videoEncoder.isEmpty() ? QStringLiteral("none") : r.videoEncoder).arg(number(r.appliedGainDb, 2)));
  return 0;
}

} // namespace sf::xport
