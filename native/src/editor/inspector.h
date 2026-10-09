#pragma once

// Properties of the selected clip(s), as a flat value map for the UI plus the edits that go back
// through Project::apply. The reference's inspector edits text content/size/alignment/colour,
// volume/mute, speed and opacity; this covers those and the rest of the model the renderer
// already draws (transform, fades, caption and shape props, colour effects). With nothing selected
// it edits the project itself (name, canvas, frame rate, background).
//
// Edits come in two flavours: edit() is one undo step; drag() applies live while a slider moves and
// every value of the drag joins one undo group that endDrag() closes.

#include "editor/project.h"

#include <QJsonObject>
#include <functional>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

namespace sf::editor {

class Inspector : public QObject {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY changed)
  Q_PROPERTY(QString itemType READ itemType NOTIFY changed)
  Q_PROPERTY(QVariantMap values READ values NOTIFY changed)
  Q_PROPERTY(QVariantList effects READ effects NOTIFY changed)
  Q_PROPERTY(QVariantList effectCatalog READ effectCatalog CONSTANT)

public:
  explicit Inspector(Project& project, QObject* parent = nullptr);

  int count() const { return static_cast<int>(ids_.size()); }
  QString itemType() const { return values_.value(QStringLiteral("type")).toString(); }
  const QVariantMap& values() const { return values_; }
  const QVariantList& effects() const { return effects_; }
  QVariantList effectCatalog() const;

  // ---- selected clips ----
  Q_INVOKABLE bool edit(const QString& key, const QVariant& value);
  Q_INVOKABLE bool drag(const QString& key, const QVariant& value);
  Q_INVOKABLE void endDrag();

  Q_INVOKABLE bool addEffect(const QString& type);
  Q_INVOKABLE bool removeEffect(const QString& effectId);
  Q_INVOKABLE bool setEffectParam(const QString& effectId, double value, bool live = false);

  // ---- project settings ----
  Q_INVOKABLE bool editProject(const QString& key, const QVariant& value);
  Q_INVOKABLE QVariantMap projectValues() const;

signals:
  void changed();

private:
  void refresh();
  bool run(const QString& key, const QVariant& value, bool live);
  // The ops that set `key` on `item`; false when the clip has no such property.
  bool opsFor(const Item& item, const QString& key, const QVariant& value, std::vector<Op>& out) const;
  bool propsOp(const Item& item, const std::function<bool(QJsonObject&)>& change, std::vector<Op>& out) const;
  QString labelFor(const QString& key) const;

  Project& project_;
  std::vector<QString> ids_;
  QVariantMap values_;
  QVariantList effects_;
  bool dragging_ = false;
};

} // namespace sf::editor
