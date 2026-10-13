#include "app/colour_page.h"

#include "app/editor.h"
#include "core/ops.h"
#include "core/timeline_doc.h"

#include <QFileInfo>
#include <QAbstractItemModel>

#include <algorithm>

namespace sf::app {

using editor::Project;

ColourPage::ColourPage(Project& project, editor::MediaPool* pool, Player* player, QObject* parent) : QObject(parent), project_(project), pool_(pool), player_(player) {
  connect(&project_, &Project::docChanged, this, &ColourPage::refresh);
  connect(&project_, &Project::selectionChanged, this, [this] {
    adoptSelection();
    refresh();
  });
  if (pool_) {
    connect(pool_, &QAbstractItemModel::dataChanged, this, &ColourPage::refresh);
    connect(pool_, &QAbstractItemModel::modelReset, this, &ColourPage::refresh);
  }
  if (player_) connect(player_, &Player::colourBypassChanged, this, &ColourPage::bypassChanged);
  adoptSelection();
  refresh();
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
