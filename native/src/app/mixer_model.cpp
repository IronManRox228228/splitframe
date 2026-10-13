#include "app/mixer_model.h"

#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "core/ops.h"
#include "core/timeline_doc.h"

#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace sf::app {

using editor::Project;

namespace {

constexpr double kNoReading = MixerModel::kFloorDb - 40;

double peakDb(float linear) { return linear <= 0.f ? MixerModel::kFloorDb : std::max(MixerModel::kFloorDb, 20.0 * std::log10(static_cast<double>(linear))); }

bool hasSound(TrackKind k) { return k != TrackKind::Text; }

} // namespace

double MixerModel::dbOf(double linear) { return linear <= 0 ? kFloorDb : std::max(kFloorDb, 20.0 * std::log10(linear)); }
double MixerModel::linearOf(double db) { return db <= kFloorDb ? 0.0 : std::pow(10.0, std::min(db, kMaxDb) / 20.0); }

MixerModel::MixerModel(Project& project, QObject* parent) : QAbstractListModel(parent), project_(project) {
  connect(&project_, &Project::docChanged, this, &MixerModel::refresh);
  timer_.setInterval(33);
  connect(&timer_, &QTimer::timeout, this, &MixerModel::tick);
  refresh();
}

MixerModel::~MixerModel() {
  if (worker_) {
    worker_->wait();
    delete worker_;
  }
}

void MixerModel::setPlayer(Player* player) { player_ = player; }

int MixerModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(strips_.size()); }

QHash<int, QByteArray> MixerModel::roleNames() const {
  return {{TrackIdRole, "trackId"}, {NameRole, "name"},       {KindRole, "kind"},   {GainDbRole, "gainDb"}, {PanRole, "pan"},     {MutedRole, "muted"},
          {SoloRole, "solo"},       {InsertsRole, "inserts"}, {PeakLRole, "peakL"}, {PeakRRole, "peakR"},   {HoldLRole, "holdL"}, {HoldRRole, "holdR"}};
}

QVariant MixerModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
  const Strip& s = strips_[static_cast<size_t>(index.row())];
  switch (role) {
  case TrackIdRole: return s.id;
  case NameRole:
  case Qt::DisplayRole: return s.name;
  case KindRole: return s.kind;
  case GainDbRole: return dbOf(s.volume);
  case PanRole: return s.pan;
  case MutedRole: return s.muted;
  case SoloRole: return s.solo;
  case InsertsRole: return s.inserts;
  case PeakLRole: return peakDb(s.peak[0]);
  case PeakRRole: return peakDb(s.peak[1]);
  case HoldLRole: return peakDb(s.hold[0]);
  case HoldRRole: return peakDb(s.hold[1]);
  default: return {};
  }
}

QVariantList MixerModel::insertsOf(const MixNode& node) {
  QVariantList out;
  for (const MixInsert& i : node.inserts)
    out.push_back(QVariantMap{{QStringLiteral("id"), i.id}, {QStringLiteral("type"), i.type}, {QStringLiteral("bypass"), i.bypass}});
  return out;
}

MixerModel::Strip MixerModel::stripFor(const Track& track, const MixNode* node) {
  Strip s;
  s.id = track.id;
  s.name = node && node->name ? *node->name : track.name;
  s.kind = enumName(track.kind);
  if (node) {
    s.volume = node->volume;
    s.pan = node->pan;
    s.muted = node->muted;
    s.solo = node->solo;
    s.inserts = insertsOf(*node);
  }
  return s;
}

void MixerModel::refresh() {
  const TimelineDoc& doc = project_.doc();
  const Mixer* mixer = doc.mixer ? &*doc.mixer : nullptr;
  std::vector<Strip> next;
  bool solo = false;
  for (const Track& t : doc.tracks) {
    if (!hasSound(t.kind)) continue;
    const MixNode* node = nullptr;
    if (mixer)
      for (const MixNode& n : mixer->strips)
        if (n.id == t.id) node = &n;
    next.push_back(stripFor(t, node));
    solo = solo || next.back().solo;
  }
  const MixNode m = mixer ? mixer->master : MixNode{.id = QStringLiteral("master")};
  QVariantMap masterNext{{QStringLiteral("gainDb"), dbOf(m.volume)}, {QStringLiteral("pan"), m.pan}, {QStringLiteral("muted"), m.muted}, {QStringLiteral("inserts"), insertsOf(m)}};
  for (const char* k : {"peakL", "peakR", "holdL", "holdR"}) masterNext.insert(QLatin1String(k), master_.value(QLatin1String(k), kFloorDb));
  if (masterNext != master_) {
    master_ = masterNext;
    emit masterChanged();
  }
  if (solo != anySolo_) {
    anySolo_ = solo;
    emit anySoloChanged();
  }

  const bool sameIds = next.size() == strips_.size() && std::equal(next.begin(), next.end(), strips_.begin(), [](const Strip& a, const Strip& b) { return a.id == b.id; });
  if (!sameIds) {
    beginResetModel();
    strips_ = std::move(next);
    endResetModel();
    return;
  }
  for (size_t i = 0; i < strips_.size(); ++i) {
    if (strips_[i].sameSettings(next[i])) continue;
    std::copy(std::begin(strips_[i].peak), std::end(strips_[i].peak), std::begin(next[i].peak)); // the meters carry on
    std::copy(std::begin(strips_[i].hold), std::end(strips_[i].hold), std::begin(next[i].hold));
    strips_[i] = std::move(next[i]);
    const QModelIndex idx = index(static_cast<int>(i));
    emit dataChanged(idx, idx);
  }
}

// ---------- editing ----------

bool MixerModel::run(const QString& nodeId, const QString& key, const QVariant& value, bool live) {
  const TimelineDoc& doc = project_.doc();
  Mixer m = doc.mixer.value_or(Mixer{});
  MixNode* node = nullptr;
  if (nodeId == QLatin1String("master")) {
    node = &m.master;
  } else {
    for (MixNode& n : m.strips)
      if (n.id == nodeId) node = &n;
    if (!node) {
      const auto t = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& tr) { return tr.id == nodeId && hasSound(tr.kind); });
      if (t == doc.tracks.end()) return false;
      m.strips.push_back(MixNode{.id = nodeId});
      node = &m.strips.back();
    }
  }
  QString label;
  if (key == QLatin1String("gainDb")) {
    const double v = linearOf(value.toDouble());
    if (v == node->volume) return false;
    node->volume = v;
    label = QStringLiteral("Mixer fader");
  } else if (key == QLatin1String("pan")) {
    const double v = std::clamp(value.toDouble(), -1.0, 1.0);
    if (v == node->pan) return false;
    node->pan = v;
    label = QStringLiteral("Mixer pan");
  } else if (key == QLatin1String("muted")) {
    if (value.toBool() == node->muted) return false;
    node->muted = value.toBool();
    label = node->muted ? QStringLiteral("Mute track") : QStringLiteral("Unmute track");
  } else if (key == QLatin1String("solo")) {
    if (value.toBool() == node->solo) return false;
    node->solo = value.toBool();
    label = QStringLiteral("Solo track");
  } else {
    return false;
  }
  if (live && !dragging_) {
    project_.beginGroup(label);
    dragging_ = true;
  }
  return project_.apply(Op(MixerSet{m}), label);
}

bool MixerModel::edit(const QString& nodeId, const QString& key, const QVariant& value) {
  endDrag();
  return run(nodeId, key, value, false);
}

bool MixerModel::drag(const QString& nodeId, const QString& key, const QVariant& value) { return run(nodeId, key, value, true); }

void MixerModel::endDrag() {
  if (!dragging_) return;
  dragging_ = false;
  project_.endGroup();
}

bool MixerModel::setInsertBypass(const QString& nodeId, const QString& insertId, bool bypass) {
  endDrag();
  Mixer m = project_.doc().mixer.value_or(Mixer{});
  MixNode* node = nodeId == QLatin1String("master") ? &m.master : nullptr;
  for (MixNode& n : m.strips)
    if (n.id == nodeId) node = &n;
  if (!node) return false;
  for (MixInsert& i : node->inserts) {
    if (i.id != insertId || i.bypass == bypass) continue;
    i.bypass = bypass;
    return project_.apply(Op(MixerSet{m}), bypass ? QStringLiteral("Bypass insert") : QStringLiteral("Enable insert"));
  }
  return false;
}

// ---------- meters and loudness ----------

void MixerModel::setMetering(bool on) {
  if (on == timer_.isActive()) return;
  if (on) timer_.start();
  else timer_.stop();
  emit meteringChanged();
}

void MixerModel::tick() {
  if (!player_) return;
  const bool playing = player_->playing();
  if (!playing && !wasPlaying_) return; // idle: nothing moves
  wasPlaying_ = playing;                // one more pass after stopping clears the meters
  audio::MeterSnapshot snap;
  momentary_ = kNoReading;
  if (playing) {
    snap = player_->audio().meters();
    const double m = player_->audio().loudness().momentary;
    if (std::isfinite(m)) momentary_ = m;
  }
  const auto read = [&](const QString& id, float* peak, float* hold) {
    const auto it = snap.find(id);
    for (int c = 0; c < 2; ++c) {
      peak[c] = it == snap.end() ? 0.f : it->second.peak[c];
      hold[c] = it == snap.end() ? 0.f : it->second.peakHold[c];
    }
  };
  for (Strip& s : strips_) read(s.id, s.peak, s.hold);
  if (!strips_.empty()) emit dataChanged(index(0), index(static_cast<int>(strips_.size()) - 1), {PeakLRole, PeakRRole, HoldLRole, HoldRRole});
  float peak[2], hold[2];
  read(QStringLiteral("master"), peak, hold);
  master_.insert(QStringLiteral("peakL"), peakDb(peak[0]));
  master_.insert(QStringLiteral("peakR"), peakDb(peak[1]));
  master_.insert(QStringLiteral("holdL"), peakDb(hold[0]));
  master_.insert(QStringLiteral("holdR"), peakDb(hold[1]));
  emit masterChanged();
  emit metersChanged();
}

void MixerModel::analyze() {
  if (analysing_) return;
  const TimelineDoc doc = project_.doc();
  std::map<QString, audio::FileProvider::File> files;
  for (const Asset& a : project_.assets()) files[a.id] = {a.path, a.kind != AssetKind::Image};
  analysing_ = true;
  emit analysingChanged();
  if (worker_) {
    worker_->wait();
    delete worker_;
  }
  QPointer<MixerModel> self(this);
  worker_ = QThread::create([self, doc, files = std::move(files)]() mutable {
    auto sources = std::make_shared<audio::FileProvider>();
    sources->setFiles(std::move(files));
    audio::AudioEngine engine(std::make_shared<const TimelineDoc>(doc), sources);
    const std::int64_t total = audio::frameToSample(docDurationFrames(doc), doc.project.fps);
    audio::LoudnessMeter meter;
    constexpr std::int64_t kChunk = 48000;
    std::vector<float> buf(static_cast<size_t>(kChunk) * 2);
    for (std::int64_t done = 0; done < total; done += kChunk) {
      const std::int64_t n = std::min(kChunk, total - done);
      engine.render(done, n, buf.data());
      meter.process(buf.data(), n);
    }
    const double lufs = meter.integrated(), tp = meter.truePeakDb();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [self, lufs, tp] {
          if (!self) return;
          self->integrated_ = std::isfinite(lufs) ? lufs : kNoReading;
          self->truePeak_ = std::isfinite(tp) ? tp : kNoReading;
          self->analysed_ = true;
          self->analysing_ = false;
          emit self->analysisChanged();
          emit self->analysingChanged();
        },
        Qt::QueuedConnection);
  });
  worker_->start();
}

} // namespace sf::app
