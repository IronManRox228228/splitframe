#pragma once

// The Colour page's backend. Project colour management (working space, display/view, blend space) is
// written with Project::setColorManagement (native-only: no op, not undoable). The colour inputs of the
// current clip (input space, LUT file, LUT intensity) go through ItemUpdate ops, exactly what the
// item.color.set command applies, so they are undoable. Options come from the OpenColorIO config the
// preview itself uses (Player::colorManager); without OCIO the lists are empty.
//
// "Clip" here is a video or image item: the strip on the page lists them in timeline order. The current
// clip follows the project selection, and picking one in the strip selects it.

#include "app/player.h"
#include "editor/media_pool.h"
#include "editor/project.h"

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

#include <functional>

namespace sf::app {

class ColourPage : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(ColourModel) // ColourPage.qml is the page itself
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(bool available READ available CONSTANT)
  // what the OCIO config offers; "" (built-in linear Rec.709 / sRGB) is always the first working space
  Q_PROPERTY(QStringList workingSpaces READ workingSpaces NOTIFY optionsChanged)
  Q_PROPERTY(QStringList inputSpaces READ inputSpaces NOTIFY optionsChanged)
  Q_PROPERTY(QStringList displayViews READ displayViews NOTIFY optionsChanged) // "Display/View"
  // project side
  Q_PROPERTY(QString workingSpace READ workingSpace NOTIFY projectChanged)
  Q_PROPERTY(QString displayView READ displayView NOTIFY projectChanged)
  Q_PROPERTY(QString blendSpace READ blendSpace NOTIFY projectChanged) // "linear" | "display"
  // the strip: [{id, name, thumb, start, duration, hasColour, selected}]
  Q_PROPERTY(QVariantList clips READ clips NOTIFY clipsChanged)
  // current clip
  Q_PROPERTY(QString clipId READ clipId NOTIFY clipChanged)
  Q_PROPERTY(QString clipName READ clipName NOTIFY clipChanged)
  Q_PROPERTY(QString inputSpace READ inputSpace NOTIFY clipChanged)
  Q_PROPERTY(QString lutPath READ lutPath NOTIFY clipChanged)
  Q_PROPERTY(QString lutName READ lutName NOTIFY clipChanged)
  Q_PROPERTY(double lutIntensity READ lutIntensity NOTIFY clipChanged)
  Q_PROPERTY(bool hasColour READ hasColour NOTIFY clipChanged)
  // primary correction as flat keys: exposure, temperature, tint, contrast, saturation,
  // lift.master / lift.r / lift.g / lift.b (also gamma.*, gain.*), offset.r / .g / .b
  Q_PROPERTY(QVariantMap primaries READ primaries NOTIFY clipChanged)
  Q_PROPERTY(bool hasPrimaries READ hasPrimaries NOTIFY clipChanged)
  // before / after: preview with all colour management off
  Q_PROPERTY(bool bypass READ bypass WRITE setBypass NOTIFY bypassChanged)

public:
  ColourPage(editor::Project& project, editor::MediaPool* pool, Player* player, QObject* parent = nullptr);

  bool available() const { return render::ColorManager::available(); }
  QStringList workingSpaces();
  QStringList inputSpaces();
  QStringList displayViews();
  QString workingSpace() const;
  QString displayView() const;
  QString blendSpace() const;
  QVariantList clips() const { return clips_; }
  QString clipId() const { return clipId_; }
  QString clipName() const;
  QString inputSpace() const;
  QString lutPath() const;
  QString lutName() const;
  double lutIntensity() const;
  bool hasColour() const;
  QVariantMap primaries() const;
  bool hasPrimaries() const;
  bool bypass() const { return player_ && player_->colourBypass(); }
  void setBypass(bool on);

  // ---- project colour management (not undoable) ----
  Q_INVOKABLE void setWorkingSpace(const QString& space);
  Q_INVOKABLE void setDisplayView(const QString& displayView);
  Q_INVOKABLE void setBlendSpace(const QString& space);

  // ---- clips ----
  Q_INVOKABLE void selectClip(const QString& itemId);
  // ---- the current clip (each one undo step; intensity drags are one) ----
  Q_INVOKABLE bool setInputSpace(const QString& space); // "" = by stream tags
  Q_INVOKABLE bool setLut(const QString& pathOrUrl);
  Q_INVOKABLE bool clearLut();
  Q_INVOKABLE bool dragIntensity(double value);
  Q_INVOKABLE void endDrag();
  Q_INVOKABLE bool resetClip(); // drops the whole colour block
  // primary correction: `key` as in the `primaries` property; a drag is one undo step (closed by endDrag)
  Q_INVOKABLE bool dragPrimary(const QString& key, double value);
  Q_INVOKABLE bool resetPrimary(const QString& key); // back to neutral, one undo step
  Q_INVOKABLE bool resetPrimaries();

signals:
  void optionsChanged();
  void projectChanged();
  void clipsChanged();
  void clipChanged();
  void bypassChanged();

private:
  void refresh();
  void adoptSelection();
  const Item* clip() const;
  bool editClip(const std::function<void(ItemColor&)>& change, const QString& label, bool live = false);
  void editProject(const std::function<void(ColorManagement&)>& change);
  void loadOptions();

  editor::Project& project_;
  QPointer<editor::MediaPool> pool_;
  QPointer<Player> player_;
  QString clipId_;
  QVariantList clips_;
  QStringList workingSpaces_, inputSpaces_, displayViews_;
  bool optionsLoaded_ = false;
  bool dragging_ = false;
  int thumbRevision_ = 0;
};

} // namespace sf::app
