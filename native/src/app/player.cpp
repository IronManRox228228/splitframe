#include "app/player.h"

#include "audio/timebase.h"
#include "core/error.h"
#include "core/timeline_doc.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sf::app {

namespace {

// FrameService is a QObject with a thread pool: always tear the media down on the UI thread, even
// when the render thread happens to drop the last reference.
std::shared_ptr<Media> makeMedia() {
  return std::shared_ptr<Media>(new Media, [](Media* m) {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
      delete m;
    } else {
      QMetaObject::invokeMethod(QCoreApplication::instance(), [m] { delete m; }, Qt::QueuedConnection);
    }
  });
}

// The clock covers one frame past the last clip, so the playhead can sit on the very end
// (where a new clip is appended, or a split-at-end is refused) and an empty project has a frame 0.
Frame clockFrames(const TimelineDoc& doc) { return docDurationFrames(doc) + 1; }

} // namespace

Player::Player(QObject* parent)
    : QObject(parent), audio_(std::make_unique<audio::PreviewAudio>()), clock_(audio_->clockSource()) {
  auto* timer = new QTimer(this);
  timer->setInterval(500);
  connect(timer, &QTimer::timeout, this, &Player::updateStats);
  timer->start();
  // the playhead moves on the render thread's schedule; the UI follows at 30 Hz
  auto* ui = new QTimer(this);
  ui->setInterval(33);
  connect(ui, &QTimer::timeout, this, &Player::refreshTransport);
  ui->start();
  // the render thread tells us when the clock moved on its own (the end was reached)
  connect(this, &Player::renderRequested, this, &Player::refreshTransport, Qt::QueuedConnection);
}

Player::~Player() = default;

void Player::syncAudioAssets(const render::AssetTable& assets) {
  std::map<QString, audio::FileProvider::File> files;
  for (const auto& [id, ref] : assets) files[id] = {ref.path, ref.kind != AssetKind::Image};
  audioFiles_->setFiles(std::move(files));
}

void Player::syncAudioSession(const TimelineDoc& doc) { audio_->setSession(std::make_shared<const TimelineDoc>(doc), audioFiles_); }

// Sound plays at 1x only; every other rate (shuttle, reverse) is silent and the clock runs on the system clock.
void Player::startAudio() {
  if (clock_.playing() && clock_.rate() == 1.0) {
    audio_->openDevice();
    audio_->play(clock_.frame());
  } else {
    audio_->pause();
  }
}

std::shared_ptr<Session> Player::session() const {
  const std::lock_guard lock(m_);
  return session_;
}

void Player::publish(std::shared_ptr<Session> s) {
  const std::lock_guard lock(m_);
  session_ = std::move(s);
}

void Player::loadProject(TimelineDoc doc, render::AssetTable assets) {
  audio_->pause();
  syncAudioAssets(assets);
  auto media = makeMedia();
  FrameService::Options o;
  o.gpuFrames = zeroCopy_;
  if (softwareDecode_) o.hw = HwMode::Off;
  media->service = std::make_unique<FrameService>(o);
  const bool few = assets.size() <= 8;
  media->provider = std::make_unique<render::FrameServiceProvider>(*media->service, std::move(assets));
  media->provider->setUseProxies(useProxies_);
  // the packet scan is the slow part and belongs before play; a library of dozens of clips opens on demand instead
  if (few) media->provider->openAll();
  connect(media->service.get(), &FrameService::frameReady, this, [this] { emit mediaReady(); }, Qt::QueuedConnection);
  // a decoder that finished opening can now be asked for the picture the paused playhead is waiting on
  connect(media->service.get(), &FrameService::assetOpened, this, [this] { emit mediaReady(); }, Qt::QueuedConnection);

  auto s = std::make_shared<Session>();
  s->media = std::move(media);
  clock_.pause();
  clock_.setTimeline(static_cast<double>(doc.project.fps), clockFrames(doc));
  clock_.seek(0);
  syncAudioSession(doc);
  s->doc = std::move(doc);
  {
    const std::lock_guard lock(m_);
    session_ = std::move(s);
    pacing_.reset();
  }
  renderedFrame_ = -1;
  renderedComplete_ = false;
  frame_ = 0;
  wasPlaying_ = false;
  emit sessionChanged();
  emit transportChanged();
  emit renderRequested();
}

void Player::setDocument(TimelineDoc doc) {
  const auto current = session();
  if (!current) return;
  auto s = std::make_shared<Session>();
  s->media = current->media;
  clock_.setTimeline(static_cast<double>(doc.project.fps), clockFrames(doc));
  syncAudioSession(doc);
  s->doc = std::move(doc);
  publish(std::move(s));
  refreshTransport();
  emit sessionChanged();
  emit renderRequested();
}

void Player::setAssets(render::AssetTable assets) {
  const auto current = session();
  if (!current) return;
  syncAudioAssets(assets);
  syncAudioSession(current->doc);
  const bool few = assets.size() <= 8;
  current->media->provider->setAssets(std::move(assets));
  if (few) current->media->provider->openAll();
  emit renderRequested();
}

void Player::setUseProxies(bool on) {
  if (on == useProxies_) return;
  useProxies_ = on;
  const auto current = session();
  if (!current) return;
  current->media->provider->setUseProxies(on);
  if (current->media->provider->assets().size() <= 8) current->media->provider->openAll();
  emit renderRequested();
}

void Player::refreshTransport() {
  const qint64 f = clock_.frame();
  const bool playing = clock_.playing();
  if (!playing && audio_->playing()) audio_->pause(); // the clock stopped on its own (end of timeline)
  if (playing && audio_->playing() && clock_.fps() > 0) { // a loop wrap moved the clock: bring the sound along
    const qint64 af = audio::sampleToFrame(audio_->positionSamples(), static_cast<std::int64_t>(clock_.fps()));
    if (std::abs(af - f) > 3) audio_->seek(f);
  }
  if (f != frame_ || playing != wasPlaying_) {
    frame_ = f;
    wasPlaying_ = playing;
    emit transportChanged();
  }
}

void Player::play() {
  if (!session_) return;
  clock_.pause();
  clock_.play(clock_.rate() > 0 ? clock_.rate() : 1.0);
  startAudio();
  refreshTransport();
  emit renderRequested();
}

void Player::pause() {
  audio_->pause();
  clock_.pause();
  refreshTransport();
  emit renderRequested();
}

void Player::toggle() {
  if (clock_.playing()) pause();
  else play();
}

void Player::seek(qint64 frame) {
  clock_.seek(frame);
  audio_->seek(clock_.frame());
  refreshTransport();
  emit renderRequested();
}

void Player::step(int frames) {
  audio_->pause();
  clock_.pause();
  clock_.seek(clock_.frame() + frames);
  refreshTransport();
  emit renderRequested();
}

void Player::shuttle(int dir) {
  if (!session_) return;
  if (dir == 0) {
    pause();
    return;
  }
  audio_->pause();
  clock_.play(render::PlaybackClock::shuttleRate(clock_.rate(), clock_.playing(), dir));
  startAudio();
  refreshTransport();
  emit renderRequested();
}

void Player::setLoop(bool loop) {
  loop_ = loop;
  clock_.setLoop(loop);
  emit transportChanged();
}

void Player::clockChanged() { QMetaObject::invokeMethod(this, &Player::refreshTransport, Qt::QueuedConnection); }

QString Player::timecode() const { return formatTimecode(frame_, std::max(1.0, clock_.fps())); }
QString Player::durationTimecode() const { return formatTimecode(std::max<qint64>(0, clock_.duration() - 1), std::max(1.0, clock_.fps())); }

void Player::notePresented(qint64 frame, bool complete, int gpuLayers, int cpuLayers, bool playing, double rate) {
  renderedFrame_ = frame;
  renderedComplete_ = complete;
  const bool layersChanged = gpuLayers_ != gpuLayers || cpuLayers_ != cpuLayers;
  gpuLayers_ = gpuLayers;
  cpuLayers_ = cpuLayers;
  {
    const std::lock_guard lock(m_);
    pacing_.present(frame, playing, rate);
  }
  if (layersChanged) QMetaObject::invokeMethod(this, &Player::updateStats, Qt::QueuedConnection);
}

qint64 Player::presentedFrames() const {
  const std::lock_guard lock(m_);
  return pacing_.presented();
}

qint64 Player::droppedFrames() const {
  const std::lock_guard lock(m_);
  return pacing_.dropped();
}

bool Player::settled() const { return session_ && renderedComplete_ && renderedFrame_ == clock_.frame() && !clock_.playing(); }

void Player::updateStats() {
  std::shared_ptr<Session> s;
  qint64 presented = 0, dropped = 0;
  {
    const std::lock_guard lock(m_);
    s = session_;
    presented = pacing_.presented();
    dropped = pacing_.dropped();
  }
  if (!s) return;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (lastStatsMs_ > 0 && now > lastStatsMs_) {
    presentedFps_ = static_cast<double>(presented - presentedAtLastStats_) * 1000.0 / static_cast<double>(now - lastStatsMs_);
  }
  presentedAtLastStats_ = presented;
  lastStatsMs_ = now;
  const QString decoders = s->media->provider ? s->media->provider->decoderSummary() : QString();
  QString text = decoders.isEmpty() ? QStringLiteral("no video") : decoders;
  text += QStringLiteral(" · %1").arg(gpuLayers_ > 0 ? QStringLiteral("zero-copy GPU") : (cpuLayers_ > 0 ? QStringLiteral("CPU upload") : QStringLiteral("no video layer")));
  if (clock_.playing()) {
    text += QStringLiteral(" · %1 fps shown · %2 dropped").arg(presentedFps_, 0, 'f', 1).arg(dropped);
    if (std::abs(clock_.rate()) != 1.0) text += QStringLiteral(" · %1×").arg(clock_.rate(), 0, 'g', 3);
  }
  if (text != stats_) {
    stats_ = text;
    emit statsChanged();
  }
}

} // namespace sf::app
