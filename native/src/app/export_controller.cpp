#include "app/export_controller.h"

#include "core/timeline_doc.h"
#include "render/media_provider.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace sf::app {

namespace {

struct Format {
  const char* label;
  xport::VideoCodec video;
  xport::Container container;
  bool tenBit;
  const char* ext;
};

const Format kFormats[] = {
    {"H.264 (MP4)", xport::VideoCodec::H264, xport::Container::Mp4, false, "mp4"},
    {"HEVC (MP4)", xport::VideoCodec::H265, xport::Container::Mp4, false, "mp4"},
    {"HEVC 10-bit (MKV)", xport::VideoCodec::H265, xport::Container::Mkv, true, "mkv"},
    {"ProRes 422 HQ (MOV)", xport::VideoCodec::ProRes, xport::Container::Mov, false, "mov"},
    {"DNxHR HQ (MOV)", xport::VideoCodec::DnxHr, xport::Container::Mov, false, "mov"},
    {"Audio only (WAV)", xport::VideoCodec::None, xport::Container::Wav, false, "wav"},
};

QString toLocal(const QString& pathOrUrl) {
  if (pathOrUrl.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) return QUrl(pathOrUrl).toLocalFile();
  return pathOrUrl;
}

} // namespace

ExportController::ExportController(editor::Project& project, QObject* parent) : QObject(parent), project_(project) {}

ExportController::~ExportController() {
  delete job_; // cancels and joins
}

QStringList ExportController::presets() const {
  QStringList l{QStringLiteral("Project size")};
  for (const xport::Preset& p : xport::listPresets()) l << p.name;
  return l;
}

QStringList ExportController::formats() const {
  QStringList l;
  for (const Format& f : kFormats) l << QString::fromLatin1(f.label);
  return l;
}

QVariantMap ExportController::defaults() const {
  const TimelineDoc& doc = project_.doc();
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  const QString base = project_.name().isEmpty() ? QStringLiteral("export") : project_.name();
  QString name = base;
  name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")), QStringLiteral("_"));
  return {{QStringLiteral("path"), QDir::toNativeSeparators(QDir(dir).absoluteFilePath(name + QStringLiteral(".mp4")))},
          {QStringLiteral("fps"), static_cast<int>(doc.project.fps)},
          {QStringLiteral("durationFrames"), static_cast<qlonglong>(docDurationFrames(doc))},
          {QStringLiteral("width"), static_cast<int>(doc.project.width)},
          {QStringLiteral("height"), static_cast<int>(doc.project.height)}};
}

std::optional<xport::ExportSettings> ExportController::settingsFor(const QVariantMap& o, QString* error) const {
  const TimelineDoc& doc = project_.doc();
  xport::ExportSettings s;
  s.outputPath = toLocal(o.value(QStringLiteral("path")).toString());
  s.overwrite = o.value(QStringLiteral("overwrite")).toBool();
  const QString formatLabel = o.value(QStringLiteral("format")).toString();
  const Format* fmt = &kFormats[0];
  for (const Format& f : kFormats) {
    if (formatLabel == QLatin1String(f.label)) fmt = &f;
  }
  s.video = fmt->video;
  s.container = fmt->container;
  s.tenBit = fmt->tenBit;
  if (s.container == xport::Container::Wav) s.audio = xport::AudioCodec::Pcm16;
  else if (s.container == xport::Container::Mov) s.audio = xport::AudioCodec::Pcm16;
  // the file's extension follows the format
  if (!s.outputPath.isEmpty() && QFileInfo(s.outputPath).suffix().toLower() != QLatin1String(fmt->ext)) {
    const QFileInfo fi(s.outputPath);
    s.outputPath = QDir(fi.absolutePath()).absoluteFilePath(fi.completeBaseName() + QLatin1Char('.') + QLatin1String(fmt->ext));
  }
  s.hardware = o.value(QStringLiteral("hardware")).toBool() ? xport::Hardware::Auto : xport::Hardware::Off;
  const QString preset = o.value(QStringLiteral("preset")).toString();
  if (!preset.isEmpty() && preset != QLatin1String("Project size") && s.video != xport::VideoCodec::None) {
    if (!xport::applyPreset(s, preset, static_cast<int>(doc.project.width), static_cast<int>(doc.project.height))) {
      if (error) *error = QStringLiteral("Unknown preset \"%1\"").arg(preset);
      return std::nullopt;
    }
  }
  const QString quality = o.value(QStringLiteral("quality")).toString();
  // a bitrate preset (platform sizes) wins over the quality levels for H.264 / HEVC, as in the Electron app
  if (!quality.isEmpty()) {
    const int bitrate = s.videoBitrateK;
    xport::applyQuality(s, quality);
    if (bitrate > 0 && quality == QLatin1String("standard")) s.videoBitrateK = bitrate;
  }
  if (o.value(QStringLiteral("customRange")).toBool()) {
    s.inFrame = o.value(QStringLiteral("rangeIn")).toLongLong();
    s.outFrame = o.value(QStringLiteral("rangeOut")).toLongLong();
  }
  if (o.value(QStringLiteral("normalize")).toBool()) s.loudnessLufs = o.value(QStringLiteral("lufs"), -14.0).toDouble();
  return s;
}

QString ExportController::summary(const QVariantMap& options) const {
  QString err;
  const auto s = settingsFor(options, &err);
  if (!s) return err;
  const auto plan = xport::planExport(project_.doc(), *s, &err);
  if (!plan) return err;
  QString text = plan->video == xport::VideoCodec::None ? QStringLiteral("Audio only") : QStringLiteral("%1x%2").arg(plan->width).arg(plan->height);
  text += QStringLiteral(" · %1 fps · %2 frames · %3 s").arg(plan->fps).arg(plan->frames).arg(static_cast<double>(plan->frames) / plan->fps, 0, 'f', 1);
  if (plan->hdr) text += QStringLiteral(" · HDR");
  if (plan->bits > 8) text += QStringLiteral(" · %1-bit").arg(plan->bits);
  for (const QString& w : plan->warnings) text += QStringLiteral("\n") + w;
  return text;
}

bool ExportController::start(const QVariantMap& options) {
  if (job_) return false;
  QString err;
  auto s = settingsFor(options, &err);
  if (!s) {
    resultOk_ = false;
    resultText_ = err;
    emit resultChanged();
    return false;
  }
  if (!xport::planExport(project_.doc(), *s, &err)) {
    resultOk_ = false;
    resultText_ = err;
    emit resultChanged();
    return false;
  }
  // always the original files: proxies are a preview matter
  render::AssetTable assets;
  for (const Asset& a : project_.assets()) {
    if (a.status != AssetStatus::Failed && !a.path.isEmpty()) assets[a.id] = {a.path, a.kind, {}};
  }
  job_ = new xport::ExportJob(project_.doc(), std::move(assets), *s, this);
  connect(job_, &xport::ExportJob::progress, this, &ExportController::onProgress, Qt::QueuedConnection);
  connect(job_, &xport::ExportJob::finished, this, &ExportController::onFinished, Qt::QueuedConnection);
  progress_ = 0;
  status_ = QStringLiteral("Starting…");
  resultText_.clear();
  resultPath_.clear();
  resultOk_ = false;
  emit runningChanged();
  emit progressChanged();
  emit resultChanged();
  job_->start();
  return true;
}

void ExportController::cancel() {
  if (job_) {
    status_ = QStringLiteral("Cancelling…");
    emit progressChanged();
    job_->cancel();
  }
}

void ExportController::onProgress(const xport::ExportProgress& p) {
  if (!job_) return;
  progress_ = p.fraction;
  const auto eta = static_cast<int>(p.etaSec + 0.5);
  status_ = p.stage == QLatin1String("finishing") ? QStringLiteral("Finishing…")
          : p.stage == QLatin1String("measuring loudness") ? QStringLiteral("Measuring loudness…")
          : QStringLiteral("Frame %1 of %2 · %3 fps · %4 left").arg(p.frame).arg(p.totalFrames).arg(p.fps, 0, 'f', 1).arg(eta >= 60 ? QStringLiteral("%1:%2").arg(eta / 60).arg(eta % 60, 2, 10, QLatin1Char('0')) : QStringLiteral("%1 s").arg(eta));
  emit progressChanged();
}

void ExportController::onFinished(const xport::ExportResult& r) {
  resultOk_ = r.ok;
  resultPath_ = r.ok ? r.path : QString();
  resultText_ = r.ok ? QStringLiteral("Exported %1 frames in %2 s (%3 fps)").arg(r.videoFrames).arg(r.seconds, 0, 'f', 1).arg(r.averageFps, 0, 'f', 1)
                     : (r.cancelled ? QStringLiteral("Export cancelled") : r.error);
  if (r.ok && !r.warnings.isEmpty()) resultText_ += QStringLiteral("\n") + r.warnings.join(QLatin1Char('\n'));
  if (r.ok && r.videoFrames == 0 && r.audioSamples > 0) resultText_ = QStringLiteral("Exported the audio in %1 s").arg(r.seconds, 0, 'f', 1);
  progress_ = r.ok ? 1.0 : progress_;
  if (job_) {
    job_->deleteLater();
    job_ = nullptr;
  }
  emit runningChanged();
  emit progressChanged();
  emit resultChanged();
}

void ExportController::reveal(const QString& path) const {
  const QString p = toLocal(path);
  if (p.isEmpty()) return;
  QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(p)});
}

void ExportController::clearResult() {
  resultText_.clear();
  resultPath_.clear();
  resultOk_ = false;
  progress_ = 0;
  status_.clear();
  emit resultChanged();
  emit progressChanged();
}

} // namespace sf::app
