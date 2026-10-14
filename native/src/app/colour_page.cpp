#include "app/colour_page.h"

#include "app/editor.h"
#include "core/ops.h"
#include "core/timeline_doc.h"
#include "render/colour_analysis.h"
#include "render/colour_capture.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QAbstractItemModel>

#include <algorithm>
#include <cmath>

namespace sf::app {

using editor::Project;

ColourPage::ColourPage(Project& project, editor::MediaPool* pool, Player* player, QObject* parent) : QObject(parent), project_(project), pool_(pool), player_(player) {
  scopesTimer_.setSingleShot(true);
  scopesTimer_.setInterval(250);
  connect(&scopesTimer_, &QTimer::timeout, this, &ColourPage::startScopes);
  connect(&project_, &Project::docChanged, this, [this] {
    refresh();
    scheduleScopes();
  });
  connect(&project_, &Project::selectionChanged, this, [this] {
    adoptSelection();
    refresh();
    scheduleScopes();
  });
  if (player_) connect(player_, &Player::transportChanged, this, &ColourPage::scheduleScopes);
  if (pool_) {
    connect(pool_, &QAbstractItemModel::dataChanged, this, &ColourPage::refresh);
    connect(pool_, &QAbstractItemModel::modelReset, this, &ColourPage::refresh);
  }
  if (player_) connect(player_, &Player::colourBypassChanged, this, &ColourPage::bypassChanged);
  adoptSelection();
  refresh();
}

// ---------- measuring (scopes, Auto balance, Match) ----------

// Renders and measures on its own thread with its own offscreen renderer (created there, destroyed there).
struct ColourPage::Worker : QObject {
  std::unique_ptr<render::OffscreenRenderer> renderer;
  render::OffscreenRenderer* get(QString* error) {
    if (!renderer) renderer = render::OffscreenRenderer::create(error);
    return renderer.get();
  }
};

namespace {

struct Outcome {
  bool ok = false;
  QString error;
  render::ColourMetrics metrics;
  PrimaryGrade grade;
  double residual = 0;
  QStringList notes;
};

render::AssetTable assetTableOf(const Project& p) {
  render::AssetTable table;
  for (const Asset& a : p.assets()) {
    if (a.status == AssetStatus::Failed || a.path.isEmpty()) continue;
    table[a.id] = {a.path, a.kind, {}};
  }
  return table;
}

QString describeGrade(const PrimaryGrade& g) {
  QStringList parts;
  const auto add = [&](const char* name, double v, double neutral, int decimals) {
    if (std::fabs(v - neutral) >= 0.5 * std::pow(10.0, -decimals)) parts << QStringLiteral("%1 %2").arg(QLatin1String(name), QString::number(v, 'f', decimals));
  };
  add("exposure", g.exposure, 0, 2);
  add("temperature", g.temperature, 0, 2);
  add("tint", g.tint, 0, 2);
  add("contrast", g.contrast, 1, 2);
  add("saturation", g.saturation, 1, 2);
  if (g.gain != Rgb3{1, 1, 1}) parts << QStringLiteral("gain %1/%2/%3").arg(g.gain.r, 0, 'f', 2).arg(g.gain.g, 0, 'f', 2).arg(g.gain.b, 0, 'f', 2);
  return parts.isEmpty() ? QStringLiteral("no change") : parts.join(QStringLiteral(", "));
}

QVariantMap scopesMap(const render::ColourMetrics& c, qint64 frame) {
  QVariantList hist;
  double peak = 0;
  for (double v : c.histogram) {
    hist << v;
    peak = std::max(peak, v);
  }
  return QVariantMap{{QStringLiteral("valid"), true},
                     {QStringLiteral("frame"), frame},
                     {QStringLiteral("histogram"), hist},
                     {QStringLiteral("histogramMax"), peak},
                     {QStringLiteral("exposure"), c.exposureStops},
                     {QStringLiteral("contrast"), c.contrastStops},
                     {QStringLiteral("crushed"), c.crushedPct.all},
                     {QStringLiteral("clipped"), c.clippedPct.all},
                     {QStringLiteral("castTemperature"), c.castTemperature},
                     {QStringLiteral("castTint"), c.castTint},
                     {QStringLiteral("skinPct"), c.skinPct},
                     {QStringLiteral("skinOffset"), c.skinOffsetDeg}};
}

} // namespace

ColourPage::~ColourPage() {
  if (!thread_) return;
  Worker* w = worker_;
  QMetaObject::invokeMethod(w, [w] { w->renderer.reset(); }, Qt::BlockingQueuedConnection);
  thread_->quit();
  thread_->wait();
  delete worker_;
  delete thread_;
}

ColourPage::Worker* ColourPage::worker() {
  if (!worker_) {
    thread_ = new QThread;
    worker_ = new Worker;
    worker_->moveToThread(thread_);
    thread_->start();
  }
  return worker_;
}

void ColourPage::setScopesActive(bool on) {
  if (on == scopesActive_) return;
  scopesActive_ = on;
  emit scopesActiveChanged();
  if (on) scheduleScopes();
}

void ColourPage::setPickingReference(bool on) {
  if (on == picking_) return;
  picking_ = on;
  emit pickingChanged();
}

void ColourPage::scheduleScopes() {
  if (scopesActive_ && player_) scopesTimer_.start();
}

void ColourPage::refreshScopes() {
  scopesKey_.clear();
  startScopes();
}

void ColourPage::startScopes() {
  if (!scopesActive_ || !player_) return;
  const Item* it = clip();
  if (!it) {
    if (!scopes_.isEmpty()) {
      scopes_.clear();
      scopesKey_.clear();
      emit scopesChanged();
    }
    return;
  }
  if (scopeJobs_ > 0) {
    scopesDirty_ = true;
    return;
  }
  Frame frame = player_->frame();
  if (frame < it->startFrame || frame >= it->startFrame + it->durationFrames) frame = it->startFrame + it->durationFrames / 2;
  const QString colourJson = QString::fromUtf8(QJsonDocument(it->color ? toJson(*it->color) : QJsonObject{}).toJson(QJsonDocument::Compact));
  const QString key = QStringLiteral("%1|%2|%3|%4").arg(it->id).arg(frame).arg(colourJson, workingSpace() + displayView() + blendSpace());
  scopesDirty_ = false;
  if (key == scopesKey_) return;

  TimelineDoc doc = render::isolateItem(project_.doc(), it->id, true);
  render::AssetTable assets = assetTableOf(project_);
  ++scopeJobs_;
  emit scopesChanged();
  QPointer<ColourPage> self(this);
  Worker* w = worker();
  QMetaObject::invokeMethod(
      w,
      [w, self, doc = std::move(doc), assets = std::move(assets), frame, key]() mutable {
        Outcome out;
        QString err;
        if (render::OffscreenRenderer* r = w->get(&err)) {
          const render::Capture cap = render::captureFrames(*r, doc, assets, {frame}, true);
          if (cap.ok()) {
            out.metrics = render::analyze(cap.frames.front());
            out.ok = out.metrics.valid;
          } else {
            out.error = cap.error;
          }
        } else {
          out.error = err;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out, key, frame] {
              if (!self) return;
              --self->scopeJobs_;
              if (out.ok) {
                self->scopes_ = scopesMap(out.metrics, frame);
                self->scopesKey_ = key;
              } else {
                self->scopes_ = QVariantMap{{QStringLiteral("valid"), false}, {QStringLiteral("error"), out.error}};
                self->scopesKey_.clear();
              }
              emit self->scopesChanged();
              if (self->scopesDirty_) self->startScopes();
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

void ColourPage::autoBalance() { startProposal(false, QString()); }

void ColourPage::matchTo(const QString& referenceItemId) {
  setPickingReference(false);
  startProposal(true, referenceItemId);
}

void ColourPage::startProposal(bool match, const QString& referenceId) {
  const Item* it = clip();
  if (!it || proposing_) return;
  const Item* ref = match ? getItem(project_.doc(), referenceId) : nullptr;
  if (match && (!ref || referenceId == it->id)) {
    status_ = QStringLiteral("Pick a different clip as the reference.");
    emit statusChanged();
    return;
  }
  proposing_ = true;
  status_ = match ? QStringLiteral("Matching...") : QStringLiteral("Measuring...");
  emit statusChanged();
  const Item copy = *it;
  const Item refCopy = ref ? *ref : Item{};
  const QString refName = ref ? editor::itemDisplayName(*ref, ref->assetId ? project_.asset(*ref->assetId) : nullptr) : QString();
  const TimelineDoc whole = project_.doc();
  render::AssetTable assets = assetTableOf(project_);
  QPointer<ColourPage> self(this);
  Worker* w = worker();
  QMetaObject::invokeMethod(
      w,
      [w, self, match, copy, refCopy, refName, whole, assets = std::move(assets)]() mutable {
        Outcome out;
        QString err;
        if (render::OffscreenRenderer* r = w->get(&err)) {
          const auto pool = [&](const Item& item, bool keepGrade) {
            render::PixelSet all;
            const render::Capture cap = render::captureFrames(*r, render::isolateItem(whole, item.id, keepGrade), assets, render::sampleFrames(item, 3), true);
            if (!cap.ok()) {
              out.error = cap.error.isEmpty() ? QStringLiteral("The clip has no pixels on the canvas.") : cap.error;
              return all;
            }
            for (const render::PixelSet& f : cap.frames) all.append(f);
            return all;
          };
          const render::PixelSet target = pool(copy, false);
          if (out.error.isEmpty()) {
            if (!match) {
              const render::Proposal p = render::proposeAutoBalance(target);
              out.grade = p.primaries;
              out.notes = p.notes;
              out.ok = true;
            } else {
              const render::PixelSet ref = pool(refCopy, true);
              if (out.error.isEmpty()) {
                const render::Proposal p = render::proposeMatch(target, render::analyze(ref));
                out.grade = p.primaries;
                out.notes = p.notes;
                out.residual = p.error;
                out.ok = true;
              }
            }
          }
        } else {
          out.error = err;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, out, match, itemId = copy.id, refName] {
              if (!self) return;
              self->proposing_ = false;
              if (!out.ok) {
                self->status_ = QStringLiteral("Could not measure: %1").arg(out.error);
              } else if (self->clipId_ != itemId) {
                self->status_ = QStringLiteral("The clip changed while measuring; nothing applied.");
              } else {
                self->endDrag();
                const PrimaryGrade g = out.grade;
                self->editClip([&](ItemColor& c) { c.primaries = g; }, match ? QStringLiteral("Match colour") : QStringLiteral("Auto balance"));
                self->status_ = match ? QStringLiteral("Matched to %1 (residual %2 stops): %3").arg(refName).arg(out.residual, 0, 'f', 2).arg(describeGrade(g))
                                      : QStringLiteral("Auto balance: %1").arg(describeGrade(g));
                if (!out.notes.isEmpty()) self->status_ += QStringLiteral(". ") + out.notes.join(QLatin1Char(' '));
              }
              emit self->statusChanged();
              self->scopesKey_.clear();
              self->scheduleScopes();
            },
            Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

// ---------- options (loaded on first ask: reading the config is not free) ----------

void ColourPage::loadOptions() {
  if (optionsLoaded_) return;
  optionsLoaded_ = true;
  workingSpaces_ = QStringList{QString()};
  if (!player_) return;
  const auto cm = player_->colorManager();
  if (!cm) return;
  inputSpaces_ = cm->colorSpaces();
  // a working space is a scene-linear space the config can convert Rec.709 into and back
  for (const QString& s : inputSpaces_) {
    std::array<double, 9> from{}, to{};
    if (cm->workingMatrix(s, &from, &to)) workingSpaces_ << s;
  }
  for (const QString& d : cm->displays())
    for (const QString& v : cm->views(d)) displayViews_ << d + QLatin1Char('/') + v;
}

QStringList ColourPage::workingSpaces() {
  loadOptions();
  return workingSpaces_;
}
QStringList ColourPage::inputSpaces() {
  loadOptions();
  return inputSpaces_;
}
QStringList ColourPage::displayViews() {
  loadOptions();
  return displayViews_;
}

// ---------- project ----------

QString ColourPage::workingSpace() const { return project_.doc().project.colorManagement.value_or(ColorManagement{}).workingSpace.value_or(QString()); }
QString ColourPage::displayView() const { return project_.doc().project.colorManagement.value_or(ColorManagement{}).displayView.value_or(QString()); }
QString ColourPage::blendSpace() const { return enumName(project_.doc().project.colorManagement.value_or(ColorManagement{}).blendSpace); }

void ColourPage::editProject(const std::function<void(ColorManagement&)>& change) {
  ColorManagement cm = project_.doc().project.colorManagement.value_or(ColorManagement{});
  const ColorManagement before = cm;
  change(cm);
  if (cm == before) return;
  // all defaults is the same as no block at all
  project_.setColorManagement(cm == ColorManagement{} ? std::nullopt : std::optional<ColorManagement>(cm));
  emit projectChanged();
}

void ColourPage::setWorkingSpace(const QString& space) {
  editProject([&](ColorManagement& cm) {
    if (space.isEmpty()) cm.workingSpace.reset();
    else cm.workingSpace = space;
  });
}

void ColourPage::setDisplayView(const QString& dv) {
  editProject([&](ColorManagement& cm) {
    if (dv.isEmpty()) cm.displayView.reset();
    else cm.displayView = dv;
  });
}

void ColourPage::setBlendSpace(const QString& space) {
  editProject([&](ColorManagement& cm) { cm.blendSpace = space == QLatin1String("display") ? BlendSpace::Display : BlendSpace::Linear; });
}

void ColourPage::setBypass(bool on) {
  if (player_) player_->setColourBypass(on);
}

// ---------- clips ----------

const Item* ColourPage::clip() const { return clipId_.isEmpty() ? nullptr : getItem(project_.doc(), clipId_); }

// The current clip follows the selection when it holds a visual item.
void ColourPage::adoptSelection() {
  for (const QString& id : project_.selection()) {
    const Item* it = getItem(project_.doc(), id);
    if (it && (it->type() == ItemType::Video || it->type() == ItemType::Image)) {
      clipId_ = id;
      return;
    }
  }
}

void ColourPage::refresh() {
  const TimelineDoc& doc = project_.doc();
  std::vector<const Item*> visual;
  for (const Item& it : doc.items)
    if (it.type() == ItemType::Video || it.type() == ItemType::Image) visual.push_back(&it);
  std::sort(visual.begin(), visual.end(), [](const Item* a, const Item* b) { return a->startFrame != b->startFrame ? a->startFrame < b->startFrame : a->id < b->id; });
  if (!clip() && !visual.empty()) clipId_ = visual.front()->id;
  if (visual.empty()) clipId_.clear();

  QVariantList list;
  for (const Item* it : visual) {
    const Asset* asset = it->assetId ? project_.asset(*it->assetId) : nullptr;
    QString thumb;
    if (asset && pool_ && pool_->preview(asset->id)) thumb = QStringLiteral("image://pool/%1?1").arg(asset->id);
    list.push_back(QVariantMap{{QStringLiteral("id"), it->id},
                               {QStringLiteral("name"), editor::itemDisplayName(*it, asset)},
                               {QStringLiteral("thumb"), thumb},
                               {QStringLiteral("start"), static_cast<qint64>(it->startFrame)},
                               {QStringLiteral("duration"), static_cast<qint64>(it->durationFrames)},
                               {QStringLiteral("hasColour"), it->color.has_value()},
                               {QStringLiteral("selected"), it->id == clipId_}});
  }
  if (list != clips_) {
    clips_ = std::move(list);
    emit clipsChanged();
  }
  emit clipChanged();
  emit projectChanged();
}

void ColourPage::selectClip(const QString& itemId) {
  const Item* it = getItem(project_.doc(), itemId);
  if (!it) return;
  endDrag();
  clipId_ = itemId;
  project_.setSelection({itemId}); // also lands in refresh() through selectionChanged
  // show the clip: park the playhead on it when it is elsewhere
  if (player_ && (player_->frame() < it->startFrame || player_->frame() >= it->startFrame + it->durationFrames)) player_->seek(it->startFrame + it->durationFrames / 2);
}

QString ColourPage::clipName() const {
  const Item* it = clip();
  if (!it) return {};
  return editor::itemDisplayName(*it, it->assetId ? project_.asset(*it->assetId) : nullptr);
}
QString ColourPage::inputSpace() const {
  const Item* it = clip();
  return it && it->color ? it->color->inputSpace.value_or(QString()) : QString();
}
QString ColourPage::lutPath() const {
  const Item* it = clip();
  return it && it->color ? it->color->lutPath.value_or(QString()) : QString();
}
QString ColourPage::lutName() const { return QFileInfo(lutPath()).fileName(); }
double ColourPage::lutIntensity() const {
  const Item* it = clip();
  return it && it->color ? it->color->lutIntensity : 1.0;
}
bool ColourPage::hasColour() const {
  const Item* it = clip();
  return it && it->color;
}

namespace {

struct PrimaryField {
  const char* key;
  double PrimaryGrade::*scalar;
  Rgb3 PrimaryGrade::*rgb;
  double Rgb3::*channel;
  double neutral, min, max;
};

// every control the Colour page exposes, with the range the schema validates against
const std::vector<PrimaryField>& primaryFields() {
  static const std::vector<PrimaryField> fields = [] {
    std::vector<PrimaryField> f{{"exposure", &PrimaryGrade::exposure, nullptr, nullptr, 0, -4, 4},
                                {"temperature", &PrimaryGrade::temperature, nullptr, nullptr, 0, -1, 1},
                                {"tint", &PrimaryGrade::tint, nullptr, nullptr, 0, -1, 1},
                                {"contrast", &PrimaryGrade::contrast, nullptr, nullptr, 1, 0, 2},
                                {"saturation", &PrimaryGrade::saturation, nullptr, nullptr, 1, 0, 2},
                                {"lift.master", &PrimaryGrade::liftMaster, nullptr, nullptr, 0, -0.5, 0.5},
                                {"gamma.master", &PrimaryGrade::gammaMaster, nullptr, nullptr, 1, 0.2, 5},
                                {"gain.master", &PrimaryGrade::gainMaster, nullptr, nullptr, 1, 0, 4}};
    const struct {
      const char* name;
      Rgb3 PrimaryGrade::*rgb;
      double neutral, min, max;
    } groups[] = {{"lift", &PrimaryGrade::lift, 0, -0.5, 0.5}, {"gamma", &PrimaryGrade::gamma, 1, 0.2, 5}, {"gain", &PrimaryGrade::gain, 1, 0, 4}, {"offset", &PrimaryGrade::offset, 0, -1, 1}};
    static std::vector<std::string> names; // keeps the c-strings alive
    names.reserve(16);
    for (const auto& g : groups) {
      const struct {
        char c;
        double Rgb3::*m;
      } ch[] = {{'r', &Rgb3::r}, {'g', &Rgb3::g}, {'b', &Rgb3::b}};
      for (const auto& c : ch) {
        names.push_back(std::string(g.name) + "." + c.c);
        f.push_back({names.back().c_str(), nullptr, g.rgb, c.m, g.neutral, g.min, g.max});
      }
    }
    return f;
  }();
  return fields;
}

const PrimaryField* findPrimary(const QString& key) {
  for (const PrimaryField& f : primaryFields())
    if (key == QLatin1String(f.key)) return &f;
  return nullptr;
}

double& primaryRef(PrimaryGrade& g, const PrimaryField& f) { return f.scalar ? g.*f.scalar : (g.*f.rgb).*f.channel; }
double primaryValue(const PrimaryGrade& g, const PrimaryField& f) { return f.scalar ? g.*f.scalar : (g.*f.rgb).*f.channel; }

} // namespace

QVariantMap ColourPage::primaries() const {
  const Item* it = clip();
  const PrimaryGrade g = it && it->color && it->color->primaries ? *it->color->primaries : PrimaryGrade{};
  QVariantMap m;
  for (const PrimaryField& f : primaryFields()) m.insert(QLatin1String(f.key), primaryValue(g, f));
  return m;
}

bool ColourPage::hasPrimaries() const {
  const Item* it = clip();
  return it && it->color && it->color->primaries;
}

// ---------- the current clip ----------

bool ColourPage::editClip(const std::function<void(ItemColor&)>& change, const QString& label, bool live) {
  const Item* it = clip();
  if (!it) return false;
  ItemColor c = it->color.value_or(ItemColor{});
  const ItemColor before = c;
  change(c);
  if (c.primaries && *c.primaries == PrimaryGrade{}) c.primaries.reset(); // neutral = absent
  if (c == before) return false;
  ItemPatch patch;
  patch.color = c; // a default block clears it
  if (live && !dragging_) {
    project_.beginGroup(label);
    dragging_ = true;
  }
  return project_.apply(Op(ItemUpdate{it->id, patch}), label);
}

bool ColourPage::setInputSpace(const QString& space) {
  endDrag();
  return editClip(
      [&](ItemColor& c) {
        if (space.isEmpty()) c.inputSpace.reset();
        else c.inputSpace = space;
      },
      QStringLiteral("Input colour space"));
}

bool ColourPage::setLut(const QString& pathOrUrl) {
  endDrag();
  const QString path = Editor::toLocalPath(pathOrUrl);
  if (path.isEmpty()) return false;
  return editClip([&](ItemColor& c) { c.lutPath = path; }, QStringLiteral("Apply LUT"));
}

bool ColourPage::clearLut() {
  endDrag();
  return editClip(
      [](ItemColor& c) {
        c.lutPath.reset();
        c.lutIntensity = 1;
      },
      QStringLiteral("Remove LUT"));
}

bool ColourPage::dragIntensity(double value) { return editClip([&](ItemColor& c) { c.lutIntensity = std::clamp(value, 0.0, 1.0); }, QStringLiteral("LUT intensity"), true); }

bool ColourPage::dragPrimary(const QString& key, double value) {
  const PrimaryField* f = findPrimary(key);
  if (!f) return false;
  return editClip(
      [&](ItemColor& c) {
        PrimaryGrade g = c.primaries.value_or(PrimaryGrade{});
        primaryRef(g, *f) = std::clamp(value, f->min, f->max);
        c.primaries = g;
      },
      QStringLiteral("Primary correction"), true);
}

bool ColourPage::resetPrimary(const QString& key) {
  endDrag();
  const PrimaryField* f = findPrimary(key);
  if (!f) return false;
  return editClip(
      [&](ItemColor& c) {
        if (!c.primaries) return;
        primaryRef(*c.primaries, *f) = f->neutral;
      },
      QStringLiteral("Reset primary"));
}

bool ColourPage::resetPrimaries() {
  endDrag();
  return editClip([](ItemColor& c) { c.primaries.reset(); }, QStringLiteral("Reset primaries"));
}

void ColourPage::endDrag() {
  if (!dragging_) return;
  dragging_ = false;
  project_.endGroup();
}

bool ColourPage::resetClip() {
  endDrag();
  return editClip([](ItemColor& c) { c = ItemColor{}; }, QStringLiteral("Reset clip colour"));
}

} // namespace sf::app
