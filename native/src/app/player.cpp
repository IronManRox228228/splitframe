#include "app/player.h"

#include "core/error.h"
#include "core/timeline_doc.h"
#include "media/probe.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace sf::app {

namespace {

// FrameService is a QObject with a thread pool: always tear a session down on the UI thread, even
// when the render thread happens to drop the last reference.
std::shared_ptr<Session> makeSession() {
  return std::shared_ptr<Session>(new Session, [](Session* s) {
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
      delete s;
    } else {
      QMetaObject::invokeMethod(QCoreApplication::instance(), [s] { delete s; }, Qt::QueuedConnection);
    }
  });
}

bool isStill(const QString& path) {
  const QByteArray suffix = QFileInfo(path).suffix().toLower().toUtf8();
  return QImageReader::supportedImageFormats().contains(suffix);
}

} // namespace

Player::Player(QObject* parent) : QObject(parent) {
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

std::shared_ptr<Session> Player::session() const {
  const std::lock_guard lock(m_);
  return session_;
}

void Player::fail(const QString& message) {
  error_ = message;
  emit sessionChanged();
}

bool Player::open(const QString& path) {
  return QFileInfo(path).suffix().compare(QLatin1String("json"), Qt::CaseInsensitive) == 0 ? openProject(path) : openMedia(path);
}

void Player::install(std::shared_ptr<Session> s, const QString& title, const QString& summary) {
  connect(s->service.get(), &FrameService::frameReady, this, [this] { emit mediaReady(); }, Qt::QueuedConnection);
  clock_.pause();
  clock_.setTimeline(static_cast<double>(s->doc.project.fps), docDurationFrames(s->doc));
  clock_.seek(0);
  {
    const std::lock_guard lock(m_);
    session_ = std::move(s);
    pacing_.reset();
  }
  renderedFrame_ = -1;
  renderedComplete_ = false;
  title_ = title;
  summary_ = summary;
  error_.clear();
  frame_ = 0;
  wasPlaying_ = false;
  emit sessionChanged();
  emit transportChanged();
  emit renderRequested();
}

bool Player::openProject(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(QStringLiteral("Can't read %1: %2").arg(path, f.errorString()));
    return false;
  }
  const QByteArray json = f.readAll();
  try {
    const QJsonDocument parsed = QJsonDocument::fromJson(json);
    auto s = makeSession();
    render::AssetTable assets;
    if (parsed.isObject() && parsed.object().contains(QStringLiteral("doc"))) {
      ProjectBundle bundle = parseProjectBundleJson(json);
      const QDir base = QFileInfo(path).absoluteDir();
      for (const Asset& a : bundle.assets) {
        // paths are absolute when the Electron app wrote them; a relative one is relative to the project file
        const QString p = QFileInfo(a.path).isAbsolute() ? a.path : base.absoluteFilePath(a.path);
        assets[a.id] = {p, a.kind};
      }
      s->doc = std::move(bundle.doc);
    } else {
      s->doc = parseTimelineDocJson(json); // bare document: no asset table, media shows as unavailable
    }
    FrameService::Options o;
    o.gpuFrames = zeroCopy_;
    s->service = std::make_unique<FrameService>(o);
    s->provider = std::make_unique<render::FrameServiceProvider>(*s->service, std::move(assets));
    // open every video asset up front: the packet scan is the slow part and belongs before play
    s->provider->openAll();
    const QString name = s->doc.project.name.isEmpty() ? QFileInfo(path).completeBaseName() : s->doc.project.name;
    const QString summary = QStringLiteral("%1×%2 · %3 fps · %4 items on %5 tracks")
                                .arg(s->doc.project.width).arg(s->doc.project.height).arg(s->doc.project.fps).arg(s->doc.items.size()).arg(s->doc.tracks.size());
    install(std::move(s), name, summary);
    return true;
  } catch (const std::exception& e) {
    fail(QStringLiteral("%1: %2").arg(path, QString::fromUtf8(e.what())));
    return false;
  }
}

bool Player::openMedia(const QString& path) {
  if (!QFileInfo::exists(path)) {
    fail(QStringLiteral("No such file: %1").arg(path));
    return false;
  }
  try {
    auto s = makeSession();
    render::AssetTable assets;
    const QString asset = QStringLiteral("ast_media");
    Frame duration;
    EmptyDocInit init;
    init.id = QStringLiteral("prj_media");
    init.name = QFileInfo(path).completeBaseName();
    QString summary;
    ItemType type = ItemType::Video;
    if (isStill(path)) {
      QImageReader r(path);
      r.setAutoTransform(true);
      const QSize size = r.size().isValid() ? r.size() : QSize(1920, 1080);
      init.width = size.width();
      init.height = size.height();
      init.fps = 30;
      duration = 5 * 30;
      type = ItemType::Image;
      assets[asset] = {path, AssetKind::Image};
      summary = QStringLiteral("image · %1×%2").arg(size.width()).arg(size.height());
    } else {
      QString err;
      const auto info = probeMedia(path, &err);
      if (!info || !info->hasVideo) {
        fail(info ? QStringLiteral("%1 has no video").arg(path) : err);
        return false;
      }
      const double rate = info->avgFps > 0 ? info->avgFps : (info->fps > 0 ? info->fps : 30.0);
      init.fps = std::clamp<std::int64_t>(jsRound(rate), 1, 240);
      init.width = info->displayWidth > 0 ? info->displayWidth : info->width;
      init.height = info->displayHeight > 0 ? info->displayHeight : info->height;
      duration = std::max<Frame>(1, msToFrames(info->durationMs, static_cast<double>(init.fps)));
      assets[asset] = {path, AssetKind::Video};
      summary = QStringLiteral("%1 · %2×%3 · %4 fps · %5")
                    .arg(info->videoCodec).arg(info->width).arg(info->height).arg(info->avgFps > 0 ? info->avgFps : info->fps, 0, 'f', 2)
                    .arg(formatTimecode(duration, static_cast<double>(init.fps)));
    }
    s->doc = createEmptyDoc(init);
    s->doc.project.styleConfig.backgroundColor = QStringLiteral("#000000");
    ItemInit item;
    item.id = QStringLiteral("itm_media");
    for (const Track& t : s->doc.tracks) {
      if (t.kind == TrackKind::Video) item.trackId = t.id;
    }
    item.assetId = asset;
    item.startFrame = 0;
    item.durationFrames = duration;
    if (type == ItemType::Video) item.sourceInFrame = 0;
    s->doc.items.push_back(createItem(type, item));

    FrameService::Options o;
    o.gpuFrames = zeroCopy_;
    s->service = std::make_unique<FrameService>(o);
    s->provider = std::make_unique<render::FrameServiceProvider>(*s->service, std::move(assets));
    s->provider->openAll();
    install(std::move(s), QFileInfo(path).fileName(), summary);
    return true;
  } catch (const std::exception& e) {
    fail(QStringLiteral("%1: %2").arg(path, QString::fromUtf8(e.what())));
    return false;
  }
}

void Player::refreshTransport() {
  const qint64 f = clock_.frame();
  const bool playing = clock_.playing();
  if (f != frame_ || playing != wasPlaying_) {
    frame_ = f;
    wasPlaying_ = playing;
    emit transportChanged();
  }
}

void Player::play() {
  if (!session_) return;
  clock_.play(clock_.rate() > 0 ? clock_.rate() : 1.0);
  refreshTransport();
  emit renderRequested();
}

void Player::pause() {
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
  refreshTransport();
  emit renderRequested();
}

void Player::step(int frames) {
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
  clock_.play(render::PlaybackClock::shuttleRate(clock_.rate(), clock_.playing(), dir));
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
QString Player::durationTimecode() const { return formatTimecode(clock_.duration(), std::max(1.0, clock_.fps())); }

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
  const QString decoders = s->provider ? s->provider->decoderSummary() : QString();
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
