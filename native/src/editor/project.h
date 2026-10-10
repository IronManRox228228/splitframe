#pragma once

// The open project: the document (timeline plus asset table), its undo history, the selection and
// the file it lives in. Every timeline change goes through apply() -> core's applyOps, so undo/redo,
// dirty tracking and (later) the agent harness all see the same operations.
//
// The Electron app keeps projects in SQLite and saves every op; the native app writes the same
// ProjectBundle JSON that core loads and saves, so it needs explicit save/dirty/recovery on top:
//   - dirty: the bundle differs from what was last saved (or was recovered from an autosave)
//   - autosave: a dirty project is written to a recovery file after a quiet period, never over the
//     user's file; saving (or discarding) removes it
//   - recovery: autosaves found at startup, or next to a file that is about to be opened

#include "core/history.h"
#include "core/ops.h"
#include "core/schema.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <optional>
#include <vector>

namespace sf::editor {

struct RecoveryEntry {
  QString autosavePath;
  QString originalPath; // empty: the project was never saved
  QString name;
  QDateTime modified;
};

class Project : public QObject {
  Q_OBJECT

public:
  explicit Project(QObject* parent = nullptr);

  // ---- document ----
  const ProjectBundle& bundle() const { return bundle_; }
  const TimelineDoc& doc() const { return bundle_.doc; }
  const std::vector<Asset>& assets() const { return bundle_.assets; }
  const Asset* asset(const QString& id) const;

  // ---- editing ----
  // Applies the ops atomically (locks enforced) and records one undo group. False, with lastError()
  // set and editRejected emitted, when an op is malformed or cannot apply.
  bool apply(const std::vector<Op>& ops, const QString& label = {});
  bool apply(const Op& op, const QString& label = {}) { return apply(std::vector<Op>{op}, label); }
  // Same, recording who made the change (history entry's actor: "user", "agent:editor", ...).
  bool apply(const std::vector<Op>& ops, const QString& label, const QString& actor);
  // The inverse ops and the actor of the newest successful apply (headless engine, API events).
  const std::vector<Op>& lastInverse() const { return lastInverse_; }
  const QString& lastActor() const { return lastActor_; }
  const History& history() const { return history_; }

  // Everything applied between begin and end is one undo step (a slider drag). Nesting is not
  // supported; begin while open closes the previous group.
  void beginGroup(const QString& label = {});
  void endGroup();
  bool groupOpen() const { return groupOpen_; }

  bool undo();
  bool redo();
  bool canUndo() const { return history_.canUndo(); }
  bool canRedo() const { return history_.canRedo(); }
  QString undoLabel() const { return history_.undoLabel().value_or(QString()); }
  const QString& lastError() const { return lastError_; }

  // Native-only project colour management (core has no op for it): applied directly, NOT undoable, marks the
  // project dirty. nullopt clears it.
  void setColorManagement(std::optional<ColorManagement> cm);

  // ---- selection ----
  const QStringList& selection() const { return selection_; }
  bool isSelected(const QString& id) const { return selection_.contains(id); }
  void setSelection(const QStringList& ids);
  void select(const QString& id, bool additive = false); // additive toggles
  void clearSelection() { setSelection({}); }

  // ---- assets (not undoable, like the reference: they are library state, not timeline state) ----
  void addAsset(const Asset& asset);
  void updateAsset(const Asset& asset);
  // Drops the asset and the history that refers to it. Timeline items using it must be removed first.
  bool removeAsset(const QString& id);

  // ---- lifecycle ----
  void newProject(const QString& name, int width = 1920, int height = 1080, int fps = 30);
  // False with *error set when the file can't be read or isn't a project. A relative asset path is
  // resolved against the project file's folder.
  bool open(const QString& path, QString* error = nullptr);
  bool save(QString* error = nullptr);
  bool saveAs(const QString& path, QString* error = nullptr);
  // Replaces the project (used by recovery and tests); history and selection restart.
  void load(ProjectBundle bundle, const QString& path, bool dirty);

  const QString& path() const { return path_; }
  QString name() const { return bundle_.doc.project.name; }
  bool isDirty() const { return dirty_; }
  // "Name" or "Name *" for the window title
  QString displayName() const;

  // ---- autosave and recovery ----
  void setRecoveryDir(const QString& dir) { recoveryDir_ = dir; }
  const QString& recoveryDir() const { return recoveryDir_; }
  // 0 disables the timer (autosaveNow still works)
  void setAutosaveInterval(int ms);
  bool autosaveNow();
  // Autosaves left behind (crash, power loss): newest first.
  QList<RecoveryEntry> recoveries() const;
  // The autosave that is newer than `path` itself, if any.
  std::optional<RecoveryEntry> recoveryFor(const QString& path) const;
  bool openRecovery(const RecoveryEntry& entry, QString* error = nullptr);
  void discardRecovery(const RecoveryEntry& entry) const;
  void clearOwnRecovery(); // this project's autosave

signals:
  void docChanged();
  void historyChanged(); // can-undo/can-redo/label may differ
  // After a successful apply (not undo/redo): what was applied and the group label.
  void opsApplied(const std::vector<sf::Op>& ops, const QString& label);
  void editRejected(const QString& message);
  void selectionChanged();
  void assetsChanged();
  void dirtyChanged();
  void pathChanged();
  void loaded(); // new/open/recover replaced the whole project

private:
  void changed(bool docTouched);
  void refreshDirty();
  void pruneSelection();
  bool writeTo(const QString& path, QString* error);
  QString recoveryPathFor(const QString& projectId) const;

  ProjectBundle bundle_;
  ProjectBundle saved_;
  History history_;
  bool groupOpen_ = false;
  QStringList selection_;
  QString path_;
  QString lastError_;
  std::vector<Op> lastInverse_;
  QString lastActor_;
  bool dirty_ = false;
  bool forceDirty_ = false;
  QString recoveryDir_;
  QTimer autosaveTimer_;
};

} // namespace sf::editor
