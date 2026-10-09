#pragma once

// The editor facade QML talks to: owns the open project and everything built on it (media pool,
// timeline models, controller, inspector, recent files) and keeps the Player in step with the
// document. Anything that needs a dialog (save as, unsaved changes) is a signal the QML answers.

#include "app/player.h"
#include "editor/inspector.h"
#include "editor/media_pool.h"
#include "editor/project.h"
#include "editor/recent_projects.h"
#include "editor/timeline_controller.h"
#include "editor/timeline_model.h"

#include <QObject>
#include <QQmlEngine>
#include <QUrl>

namespace sf::app {

class Editor : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("created by the application")

  Q_PROPERTY(sf::editor::Project* project READ project CONSTANT)
  Q_PROPERTY(sf::editor::MediaPool* pool READ pool CONSTANT)
  Q_PROPERTY(sf::editor::TimelineModel* timeline READ timeline CONSTANT)
  Q_PROPERTY(sf::editor::TrackModel* tracks READ tracks CONSTANT)
  Q_PROPERTY(sf::editor::TimelineController* controller READ controller CONSTANT)
  Q_PROPERTY(sf::editor::Inspector* inspector READ inspector CONSTANT)
  Q_PROPERTY(sf::editor::RecentProjects* recent READ recent CONSTANT)
  Q_PROPERTY(sf::app::Player* player READ player CONSTANT)
  Q_PROPERTY(QString windowTitle READ windowTitle NOTIFY titleChanged)
  Q_PROPERTY(QString projectName READ projectName NOTIFY titleChanged)
  Q_PROPERTY(QString projectPath READ projectPath NOTIFY titleChanged)
  Q_PROPERTY(bool dirty READ dirty NOTIFY titleChanged)
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
  Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY historyChanged)
  Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
  Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)

public:
  struct Options {
    QString settingsFile;  // ini file instead of the registry (tests, scripted runs)
    QString recoveryDir;   // autosave folder
    int autosaveMs = 20000;
  };

  explicit Editor(Player& player, const Options& options, QObject* parent = nullptr);

  editor::Project* project() { return &project_; }
  editor::MediaPool* pool() { return &pool_; }
  editor::TimelineModel* timeline() { return &timeline_; }
  editor::TrackModel* tracks() { return &tracks_; }
  editor::TimelineController* controller() { return &controller_; }
  editor::Inspector* inspector() { return &inspector_; }
  editor::RecentProjects* recent() { return &recent_; }
  Player* player() { return &player_; }

  QString windowTitle() const;
  QString projectName() const { return project_.name(); }
  QString projectPath() const { return project_.path(); }
  bool dirty() const { return project_.isDirty(); }
  bool canUndo() const { return project_.canUndo(); }
  bool canRedo() const { return project_.canRedo(); }
  QString undoLabel() const { return project_.undoLabel(); }
  bool hasSelection() const { return !project_.selection().isEmpty(); }
  int selectionCount() const { return static_cast<int>(project_.selection().size()); }

  // ---- project lifecycle (callers handle unsaved changes first: see dirty) ----
  Q_INVOKABLE void newProject(const QString& name = QString(), int width = 1920, int height = 1080, int fps = 30);
  // A project .json, or a media file wrapped as a one-clip project. False with a toast on failure.
  Q_INVOKABLE bool openPath(const QString& pathOrUrl);
  Q_INVOKABLE bool save();   // false when it needs a file name first (saveAsRequested is emitted)
  Q_INVOKABLE bool saveAs(const QString& pathOrUrl);
  // Autosaves left behind by a crash: [{autosavePath, name, originalPath, modified}]
  Q_INVOKABLE QVariantList recoveries() const;
  Q_INVOKABLE bool restoreRecovery(const QString& autosavePath);
  Q_INVOKABLE void discardRecovery(const QString& autosavePath);
  // The autosave next to `path`, if newer than the file ("" when none)
  Q_INVOKABLE QString recoveryFor(const QString& path) const;

  // ---- media ----
  Q_INVOKABLE void importFiles(const QVariantList& pathsOrUrls);

  // ---- editing shortcuts that span models ----
  Q_INVOKABLE void undo();
  Q_INVOKABLE void redo();
  Q_INVOKABLE void seek(qint64 frame);
  Q_INVOKABLE void step(qint64 frames);
  Q_INVOKABLE void gotoEdge(int dir);
  Q_INVOKABLE void toast(const QString& text, const QString& kind = QStringLiteral("info"));

  static QString toLocalPath(const QString& pathOrUrl);

signals:
  void titleChanged();
  void historyChanged();
  void selectionChanged();
  void saveAsRequested();
  // kind: info, error, undo (the toast offers Undo)
  void toastRequested(const QString& text, const QString& kind);

private:
  void onLoaded();
  void pushDocument();
  void pushAssets();
  void rememberRecent();
  render::AssetTable assetTable() const;
  bool wrapMedia(const QString& path);

  Player& player_;
  editor::Project project_;
  editor::MediaPool pool_;
  editor::TimelineModel timeline_;
  editor::TrackModel tracks_;
  editor::TimelineController controller_;
  editor::Inspector inspector_;
  editor::RecentProjects recent_;
  bool adoptingPlayhead_ = false;
};

} // namespace sf::app
