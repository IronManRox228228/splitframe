#pragma once

// Preview transport: owns what is being previewed (a timeline document, its media, a FrameService)
// and the playhead clock. Lives on the UI thread; the preview item's render thread reads from it
// through the thread-safe parts (clock, session snapshot, atomics).

#include "core/schema.h"
#include "media/frame_service.h"
#include "render/media_provider.h"
#include "render/playback_clock.h"

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>

namespace sf::app {

// Everything the renderer needs for one opened document. Replaced wholesale on open; the render
// thread holds a shared_ptr snapshot, so an open never pulls the rug from under a frame in flight.
struct Session {
  TimelineDoc doc;
  std::unique_ptr<FrameService> service;
  std::unique_ptr<render::FrameServiceProvider> provider;
};

class Player : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(bool loaded READ loaded NOTIFY sessionChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
  Q_PROPERTY(qint64 frame READ frame NOTIFY transportChanged)
  Q_PROPERTY(qint64 durationFrames READ durationFrames NOTIFY sessionChanged)
  Q_PROPERTY(double fps READ fps NOTIFY sessionChanged)
  Q_PROPERTY(double rate READ rate NOTIFY transportChanged)
  Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY transportChanged)
  Q_PROPERTY(QString timecode READ timecode NOTIFY transportChanged)
  Q_PROPERTY(QString durationTimecode READ durationTimecode NOTIFY sessionChanged)
  Q_PROPERTY(QString title READ title NOTIFY sessionChanged)
  Q_PROPERTY(QString summary READ summary NOTIFY sessionChanged)
  Q_PROPERTY(QString error READ error NOTIFY sessionChanged)
  Q_PROPERTY(QString stats READ stats NOTIFY statsChanged)

public:
  explicit Player(QObject* parent = nullptr);
  ~Player() override;

  // A project JSON (ProjectBundle or bare TimelineDoc) or a single media file wrapped as a one-clip
  // document. False with error() set when it can't be opened.
  Q_INVOKABLE bool open(const QString& path);
  Q_INVOKABLE bool openProject(const QString& path);
  Q_INVOKABLE bool openMedia(const QString& path);

  Q_INVOKABLE void play();
  Q_INVOKABLE void pause();
  Q_INVOKABLE void toggle();
  Q_INVOKABLE void seek(qint64 frame);
  Q_INVOKABLE void step(int frames);
  // J / K / L: dir -1 or +1 starts or doubles the speed in that direction (1, 2, 4, 8x); a press
  // against the current direction restarts at 1x; 0 pauses.
  Q_INVOKABLE void shuttle(int dir);
  void setLoop(bool loop);
  // Zero-copy GPU video (default) or decode-download-upload; takes effect for the next open.
  void setZeroCopy(bool on) { zeroCopy_ = on; }

  bool loaded() const { return static_cast<bool>(session_); }
  bool playing() const { return clock_.playing(); }
  qint64 frame() const { return frame_; }
  qint64 durationFrames() const { return clock_.duration(); }
  double fps() const { return clock_.fps(); }
  double rate() const { return clock_.rate(); }
  bool loop() const { return loop_; }
  QString timecode() const;
  QString durationTimecode() const;
  QString title() const { return title_; }
  QString summary() const { return summary_; }
  QString error() const { return error_; }
  QString stats() const { return stats_; }
  // Frames composited / skipped (the clock moved on before the presenter got to them) since the last open
  qint64 presentedFrames() const;
  qint64 droppedFrames() const;

  // ---- render thread side ----
  render::PlaybackClock& clock() { return clock_; }
  std::shared_ptr<Session> session() const; // snapshot
  // The renderer reports every frame it composited.
  void notePresented(qint64 frame, bool complete, int gpuLayers, int cpuLayers, bool playing, double rate);
  // True once the frame the playhead is on has been composited with every picture decoded.
  bool settled() const;
  // The renderer saw the clock change state (end reached, etc.): refresh the UI side.
  void clockChanged();

signals:
  void sessionChanged();
  void transportChanged();
  void statsChanged();
  void renderRequested(); // something changed that needs a repaint
  void mediaReady();      // a decoded frame arrived

private:
  void install(std::shared_ptr<Session> s, const QString& title, const QString& summary);
  void refreshTransport();
  void updateStats();
  void fail(const QString& message);

  mutable std::mutex m_;
  std::shared_ptr<Session> session_;
  render::PlaybackClock clock_;
  render::PacingStats pacing_; // guarded by m_
  qint64 frame_ = 0;
  bool wasPlaying_ = false;
  bool loop_ = false;
  bool zeroCopy_ = true;
  QString title_, summary_, error_, stats_;
  std::atomic<qint64> renderedFrame_{-1};
  std::atomic<bool> renderedComplete_{false};
  std::atomic<int> gpuLayers_{0}, cpuLayers_{0};
  std::atomic<qint64> presentedAtLastStats_{0};
  qint64 lastStatsMs_ = 0;
  double presentedFps_ = 0;
};

} // namespace sf::app
