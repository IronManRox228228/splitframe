#include "editor/recent_projects.h"

#include <QDir>
#include <QFileInfo>

namespace sf::editor {

namespace {
const QString kKey = QStringLiteral("recentProjects");

QString canonical(const QString& path) { return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()); }
} // namespace

RecentProjects::RecentProjects(const QString& iniPath, QObject* parent) : QObject(parent) {
  if (iniPath.isEmpty()) settings_ = std::make_unique<QSettings>(QStringLiteral("SplitFrame"), QStringLiteral("SplitFrame"));
  else settings_ = std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
}

QStringList RecentProjects::paths() const { return settings_->value(kKey).toStringList(); }

QVariantList RecentProjects::entries() const {
  QVariantList out;
  for (const QString& p : paths()) {
    const QFileInfo fi(p);
    out.push_back(QVariantMap{{QStringLiteral("path"), p}, {QStringLiteral("name"), fi.completeBaseName()}, {QStringLiteral("exists"), fi.exists()}});
  }
  return out;
}

void RecentProjects::add(const QString& path) {
  const QString p = canonical(path);
  QStringList list = paths();
  list.removeAll(p);
  list.prepend(p);
  while (list.size() > kMax) list.removeLast();
  settings_->setValue(kKey, list);
  settings_->sync();
  emit changed();
}

void RecentProjects::remove(const QString& path) {
  QStringList list = paths();
  if (list.removeAll(canonical(path)) == 0) return;
  settings_->setValue(kKey, list);
  settings_->sync();
  emit changed();
}

void RecentProjects::clear() {
  settings_->remove(kKey);
  settings_->sync();
  emit changed();
}

QVariant RecentProjects::value(const QString& key, const QVariant& fallback) const { return settings_->value(key, fallback); }

void RecentProjects::setValue(const QString& key, const QVariant& v) {
  settings_->setValue(key, v);
  settings_->sync();
}

} // namespace sf::editor
