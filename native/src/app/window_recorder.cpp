#include "app/window_recorder.h"

#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sf::app {

WindowRecorder::WindowRecorder(QQuickWindow* window, Options opt, QObject* parent)
    : QObject(parent), window_(window), opt_(std::move(opt)) {
  opt_.fps = std::clamp(opt_.fps, 1, 60);
  maxFrames_ = opt_.sec > 0.0 ? static_cast<int>(std::round(opt_.sec * opt_.fps)) : 0;

  timer_ = new QTimer(this);
  timer_->setTimerType(Qt::PreciseTimer);
  const int intervalMs = std::max(1, static_cast<int>(std::round(1000.0 / opt_.fps)));
  timer_->setInterval(intervalMs);
  connect(timer_, &QTimer::timeout, this, &WindowRecorder::captureFrame);
}

WindowRecorder::~WindowRecorder() {
  stop();
}

void WindowRecorder::start() {
  if (stopped_ || !window_) return;

  if (opt_.delaySec > 0.0) {
    const int delayMs = std::max(1, static_cast<int>(std::round(opt_.delaySec * 1000.0)));
    QTimer::singleShot(delayMs, this, [this] {
      if (!stopped_) beginRecording();
    });
  } else {
    beginRecording();
  }
}

void WindowRecorder::beginRecording() {
  if (stopped_ || !window_) return;

  QDir dir(opt_.dir);
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }

  writeRecordJson();

  if (timer_) {
    timer_->start();
  }
}

void WindowRecorder::writeRecordJson() {
  if (!window_) return;

  QJsonObject obj;
  obj.insert(QStringLiteral("fps"), opt_.fps);
  obj.insert(QStringLiteral("width"), window_->width());
  obj.insert(QStringLiteral("height"), window_->height());
  obj.insert(QStringLiteral("devicePixelRatio"), window_->devicePixelRatio());

  const QString jsonPath = QDir(opt_.dir).filePath(QStringLiteral("record.json"));
  QFile file(jsonPath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();
  } else {
    std::fprintf(stderr, "record: failed to write %s\n", qPrintable(jsonPath));
  }
}

void WindowRecorder::captureFrame() {
  if (stopped_ || !window_) return;

  QImage img = window_->grabWindow();
  const QString filename = QStringLiteral("frame_%1.png").arg(framesWritten_, 6, 10, QLatin1Char('0'));
  const QString path = QDir(opt_.dir).filePath(filename);

  if (img.isNull() || !img.save(path)) {
    std::fprintf(stderr, "record: failed to save frame to %s\n", qPrintable(path));
    stop();
    return;
  }

  ++framesWritten_;
  if (maxFrames_ > 0 && framesWritten_ >= maxFrames_) {
    std::fprintf(stderr, "record: wrote %d frames to %s\n", framesWritten_, qPrintable(opt_.dir));
    std::fprintf(stdout, "record: wrote %d frames to %s\n", framesWritten_, qPrintable(opt_.dir));
    std::fflush(stderr);
    std::fflush(stdout);
    stop();
  }
}

void WindowRecorder::stop() {
  stopped_ = true;
  if (timer_) {
    timer_->stop();
  }
}

int WindowRecorder::framesWritten() const {
  return framesWritten_;
}

} // namespace sf::app
