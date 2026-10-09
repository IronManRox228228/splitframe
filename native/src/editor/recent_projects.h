#pragma once

// The most-recently-used project files, kept in QSettings. The app uses the SplitFrame/SplitFrame
// registry scope (or an ini file when --settings is given); tests pass their own QSettings.

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QVariantList>

#include <memory>

namespace sf::editor {

class RecentProjects : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)

public:
  static constexpr int kMax = 10;

  // iniPath empty: the default QSettings of the application (organisation/app set in main)
  explicit RecentProjects(const QString& iniPath = {}, QObject* parent = nullptr);

  QStringList paths() const;
  // {path, name, exists} per entry, newest first, for the Open Recent menu
  QVariantList entries() const;
  void add(const QString& path);
  void remove(const QString& path);
  Q_INVOKABLE void clear();

  // small per-user conveniences that live next to the list
  QVariant value(const QString& key, const QVariant& fallback = {}) const;
  void setValue(const QString& key, const QVariant& v);
  QString fileName() const { return settings_->fileName(); }

signals:
  void changed();

private:
  std::unique_ptr<QSettings> settings_;
};

} // namespace sf::editor
