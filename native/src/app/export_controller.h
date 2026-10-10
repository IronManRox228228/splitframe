#pragma once

// What the Export dialog talks to: turns the dialog's choices into ExportSettings, runs the export on
// its own thread (xport::ExportJob) against the project's ORIGINAL files, and reports progress.

#include "editor/project.h"
#include "export/exporter.h"

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

#include <memory>

namespace sf::app {

class ExportController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(bool running READ running NOTIFY runningChanged)
  Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
  Q_PROPERTY(QString status READ status NOTIFY progressChanged)
  Q_PROPERTY(QString resultText READ resultText NOTIFY resultChanged)
  Q_PROPERTY(bool resultOk READ resultOk NOTIFY resultChanged)
  Q_PROPERTY(QString resultPath READ resultPath NOTIFY resultChanged)
  Q_PROPERTY(QStringList presets READ presets CONSTANT)
  Q_PROPERTY(QStringList formats READ formats CONSTANT)

public:
  explicit ExportController(editor::Project& project, QObject* parent = nullptr);
  ~ExportController() override;

  bool running() const { return job_ != nullptr; }
  double progress() const { return progress_; }
  QString status() const { return status_; }
  QString resultText() const { return resultText_; }
  bool resultOk() const { return resultOk_; }
  QString resultPath() const { return resultPath_; }
  QStringList presets() const;
  QStringList formats() const;

  // {path, fps, durationFrames, width, height}: starting values for the dialog
  Q_INVOKABLE QVariantMap defaults() const;
  // One line for the dialog ("1920x1080 · 30 fps · 90 frames · 3.0 s") or the reason it can't export.
  // options: preset, format (an entry of `formats`), quality, path, customRange, rangeIn, rangeOut, normalize, lufs, hardware
  Q_INVOKABLE QString summary(const QVariantMap& options) const;
  Q_INVOKABLE bool start(const QVariantMap& options);
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void reveal(const QString& path) const;
  Q_INVOKABLE void clearResult();

  // Settings for the dialog's options, or nullopt with *error.
  std::optional<xport::ExportSettings> settingsFor(const QVariantMap& options, QString* error) const;

signals:
  void runningChanged();
  void progressChanged();
  void resultChanged();

private:
  void onProgress(const xport::ExportProgress& p);
  void onFinished(const xport::ExportResult& r);

  editor::Project& project_;
  xport::ExportJob* job_ = nullptr;
  double progress_ = 0;
  QString status_;
  QString resultText_;
  QString resultPath_;
  bool resultOk_ = false;
};

} // namespace sf::app
