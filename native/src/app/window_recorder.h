#pragma once

#include <QDir>
#include <QObject>
#include <QQuickWindow>
#include <QString>
#include <QTimer>

namespace sf::app {

class WindowRecorder : public QObject {
  Q_OBJECT

public:
  struct Options {
    QString dir;
    int fps = 30;
    double sec = 0.0;
    double delaySec = 0.0;
  };

  WindowRecorder(QQuickWindow* window, Options opt, QObject* parent = nullptr);
  ~WindowRecorder() override;

  void start();
  void stop();
  int framesWritten() const;

private slots:
  void captureFrame();

private:
  void beginRecording();
  void writeRecordJson();

  QQuickWindow* window_ = nullptr;
  Options opt_;
  QTimer* timer_ = nullptr;
  int framesWritten_ = 0;
  int maxFrames_ = 0;
  bool stopped_ = false;
};

} // namespace sf::app
