#pragma once

// Preview transport: owns what is being previewed (a timeline document, its media, a FrameService)
// and the playhead clock. Lives on the UI thread; the preview item's render thread reads from it
// through the thread-safe parts (clock, session snapshot, atomics).

#include "audio/preview_audio.h"
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

// The decoders and frame cache behind a document. Shared between the successive sessions of one
// project: an edit swaps the document without reopening any media.
struct Media {
  std::unique_ptr<FrameService> service;
  std::unique_ptr<render::FrameServiceProvider> provider;
};

// Everything the renderer needs for one state of the document. Replaced wholesale on every edit and
// on open; the render thread holds a shared_ptr snapshot, so a change never pulls the rug from
// under a frame in flight.
struct Session {
  TimelineDoc doc;
  std::shared_ptr<Media> media;
};

class Player : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(bool loaded READ loaded NOTIFY sessionChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY transportChanged)
  Q_PROPERTY(qint64 frame READ frame NOTIFY transportChanged)
  // the clock runs one frame past the last clip so the playhead can rest on the end
  Q_PROPERTY(qint64 durationFrames READ durationFrames NOTIFY sessionChanged)
  Q_PROPERTY(double fps READ fps NOTIFY sessionChanged)
  Q_PROPERTY(double rate READ rate NOTIFY transportChanged)
  Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY transportChanged)
  Q_PROPERTY(QString timecode READ timecode NOTIFY transportChanged)
  Q_PROPERTY(QString durationTimecode READ durationTimecode NOTIFY sessionChanged)
  Q_PROPERTY(QString stats READ stats NOTIFY statsChanged)

public:
  explicit Player(QObject* parent = nullptr);
  ~Player() override;

  // A new project: fresh decoders for `assets`, the playhead back at the start.
  void loadProject(TimelineDoc doc, render::AssetTable assets);
  // The document changed (an edit, undo/redo): show it from the next frame on. The playhead stays.
  void setDocument(TimelineDoc doc);
  // Media imported, removed or relinked.
  void setAssets(render::AssetTable assets);

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
  // Software decoding only (no hardware decoder sessions at all); takes effect for the next open.
  void setSoftwareDecode(bool on) { softwareDecode_ = on; }
  // Preview from the assets' proxy files (AssetRef::proxyPath) where they exist. Export never does.
  void setUseProxies(bool on);
  bool useProxies() const { return useProxies_; }

  bool loaded() const { return static_cast<bool>(session_); }
  bool playing() const { return clock_.playing(); }
  qint64 frame() const { return frame_; }
  qint64 durationFrames() const { return clock_.duration(); }
  double fps() const { return clock_.fps(); }
  double rate() const { return clock_.rate(); }
  bool loop() const { return loop_; }
  QString timecode() const;
  QString durationTimecode() const;
  QString stats() const { return stats_; }
  // Frames composited / skipped (the clock moved on before the presenter got to them) since the last open
  qint64 presentedFrames() const;
  qint64 droppedFrames() const;

  audio::PreviewAudio& audio() { return *audio_; } // meters, loudness, device state

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
  void refreshTransport();
  void updateStats();
  void publish(std::shared_ptr<Session> s);
  void syncAudioAssets(const render::AssetTable& assets);
  void syncAudioSession(const TimelineDoc& doc);
  void startAudio();

  // Sound: the engine, the sound card and the master clock the PlaybackClock follows while audio plays at 1x.
  // Declared before clock_ because the clock is built from the audio clock source.
  std::unique_ptr<audio::PreviewAudio> audio_;
  std::shared_ptr<audio::FileProvider> audioFiles_ = std::make_shared<audio::FileProvider>();
  mutable std::mutex m_;
  std::shared_ptr<Session> session_;
  render::PlaybackClock clock_;
  render::PacingStats pacing_; // guarded by m_
  qint64 frame_ = 0;
  bool wasPlaying_ = false;
  bool loop_ = false;
  bool zeroCopy_ = true;
  bool softwareDecode_ = false;
  bool useProxies_ = false;
  QString stats_;
  std::atomic<qint64> renderedFrame_{-1};
  std::atomic<bool> renderedComplete_{false};
  std::atomic<int> gpuLayers_{0}, cpuLayers_{0};
  std::atomic<qint64> presentedAtLastStats_{0};
  qint64 lastStatsMs_ = 0;
  double presentedFps_ = 0;
};

} // namespace sf::app
