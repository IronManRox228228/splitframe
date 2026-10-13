#pragma once

#include "copilot/types.h"
#include "core/schema.h"
#include "editor/project.h"

#include <QObject>
#include <map>
#include <vector>

namespace sf::copilot {

class BranchManager : public QObject {
  Q_OBJECT

public:
  explicit BranchManager(editor::Project* project = nullptr, QObject* parent = nullptr);

  void attachProject(editor::Project* project);
  // Forgets the project and its branches (a different project was opened into the same object): the next attachProject reloads.
  void detachProject();
  editor::Project* project() const { return project_; }

  QString activeBranch() const { return activeBranch_; }
  bool createBranch(const QString& name, const QString& author = QStringLiteral("user"), const QString& base = QString());
  bool switchBranch(const QString& name);
  bool deleteBranch(const QString& name);

  std::vector<BranchRecord> listBranches() const;
  std::optional<BranchRecord> getBranch(const QString& name) const;

  BranchDiff diffBranches(const QString& nameA, const QString& nameB) const;
  bool mergeBranch(const QString& sourceBranch, const QString& strategy = QStringLiteral("all"));

  // In-memory or bundle persistence helper
  void syncToBundle(ProjectBundle& bundle) const;
  void loadFromBundle(const ProjectBundle& bundle);

signals:
  void branchCreated(const QString& name);
  void branchSwitched(const QString& name);
  void branchDeleted(const QString& name);
  void branchMerged(const QString& source, const QString& target);

private:
  void saveCurrentStateToActiveBranch();

  editor::Project* project_ = nullptr;
  QString activeBranch_ = QStringLiteral("main");
  std::map<QString, BranchRecord> branches_;
};

} // namespace sf::copilot
