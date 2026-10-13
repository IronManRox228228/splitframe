#include "copilot/branch_manager.h"
#include "core/ops.h"
#include "core/time.h"

#include <QDateTime>
#include <algorithm>
#include <set>

namespace sf::copilot {

BranchManager::BranchManager(editor::Project* project, QObject* parent)
    : QObject(parent), project_(nullptr) {
  if (project) attachProject(project);
}

void BranchManager::detachProject() {
  project_ = nullptr;
  branches_.clear();
  activeBranch_ = QStringLiteral("main");
}

void BranchManager::attachProject(editor::Project* project) {
  if (project_ == project && !branches_.empty()) return;
  project_ = project;
  if (!project_) return;

  loadFromBundle(project_->bundle());

  // Ensure default main branch exists
  if (branches_.empty()) {
    BranchRecord mainRec;
    mainRec.id = QStringLiteral("brn_main");
    mainRec.name = QStringLiteral("main");
    mainRec.author = QStringLiteral("user");
    mainRec.baseBranch = QStringLiteral("main");
    mainRec.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    mainRec.updatedAt = mainRec.createdAt;
    mainRec.doc = project_->doc();
    branches_[mainRec.name] = mainRec;
    activeBranch_ = QStringLiteral("main");
  }
}

void BranchManager::saveCurrentStateToActiveBranch() {
  if (!project_) return;
  auto it = branches_.find(activeBranch_);
  if (it != branches_.end()) {
    it->second.doc = project_->doc();
    it->second.updatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  }
}

bool BranchManager::createBranch(const QString& name, const QString& author, const QString& base) {
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty()) return false;
  if (branches_.find(trimmed) != branches_.end()) return false;

  saveCurrentStateToActiveBranch();

  TimelineDoc baseDoc;
  QString baseBranchName = base.isEmpty() ? activeBranch_ : base;
  auto it = branches_.find(baseBranchName);
  if (it != branches_.end()) {
    baseDoc = it->second.doc;
  } else if (project_) {
    baseDoc = project_->doc();
    baseBranchName = activeBranch_;
  }

  BranchRecord rec;
  rec.id = newId(QStringLiteral("brn"));
  rec.name = trimmed;
  rec.author = author.isEmpty() ? QStringLiteral("user") : author;
  rec.baseBranch = baseBranchName;
  rec.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  rec.updatedAt = rec.createdAt;
  rec.doc = baseDoc;

  branches_[trimmed] = rec;

  if (project_) {
    std::vector<BranchRecord> list;
    for (const auto& [_, b] : branches_) list.push_back(b);
    project_->setBranches(std::move(list));
  }

  emit branchCreated(trimmed);
  return true;
}

bool BranchManager::switchBranch(const QString& name) {
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty()) return false;
  if (trimmed == activeBranch_) return true;

  auto it = branches_.find(trimmed);
  if (it == branches_.end()) return false;

  saveCurrentStateToActiveBranch();

  const BranchRecord target = it->second;
  activeBranch_ = trimmed;

  if (project_) {
    ProjectBundle b = project_->bundle();
    b.doc = target.doc;
    syncToBundle(b);
    project_->load(std::move(b), project_->path(), false);
  }

  emit branchSwitched(trimmed);
  return true;
}

bool BranchManager::deleteBranch(const QString& name) {
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty()) return false;
  if (trimmed == activeBranch_ || trimmed == QStringLiteral("main")) return false;

  auto it = branches_.find(trimmed);
  if (it == branches_.end()) return false;

  branches_.erase(it);

  if (project_) {
    std::vector<BranchRecord> list;
    for (const auto& [_, b] : branches_) list.push_back(b);
    project_->setBranches(std::move(list));
  }

  emit branchDeleted(trimmed);
  return true;
}

std::vector<BranchRecord> BranchManager::listBranches() const {
  std::vector<BranchRecord> result;
  result.reserve(branches_.size());
  for (const auto& [_, rec] : branches_) {
    result.push_back(rec);
  }
  return result;
}

std::optional<BranchRecord> BranchManager::getBranch(const QString& name) const {
  auto it = branches_.find(name.trimmed());
  if (it != branches_.end()) return it->second;
  return std::nullopt;
}

BranchDiff BranchManager::diffBranches(const QString& nameA, const QString& nameB) const {
  BranchDiff diff;
  TimelineDoc docA, docB;

  const QString trimmedA = nameA.trimmed();
  const QString trimmedB = nameB.trimmed();

  if (project_ && trimmedA == activeBranch_) docA = project_->doc();
  else {
    auto itA = branches_.find(trimmedA);
    if (itA != branches_.end()) docA = itA->second.doc;
  }

  if (project_ && trimmedB == activeBranch_) docB = project_->doc();
  else {
    auto itB = branches_.find(trimmedB);
    if (itB != branches_.end()) docB = itB->second.doc;
  }

  const auto calcDurMs = [](const TimelineDoc& d) -> qint64 {
    Frame maxF = 0;
    for (const auto& it : d.items) maxF = std::max(maxF, it.startFrame + it.durationFrames);
    const qint64 fps = d.project.fps > 0 ? d.project.fps : 30;
    return (maxF * 1000) / fps;
  };

  diff.durationDeltaMs = calcDurMs(docB) - calcDurMs(docA);

  std::map<QString, Item> itemsA, itemsB;
  for (const auto& it : docA.items) itemsA[it.id] = it;
  for (const auto& it : docB.items) itemsB[it.id] = it;

  for (const auto& [id, it] : itemsB) {
    if (itemsA.find(id) == itemsA.end()) {
      diff.addedItemIds.append(id);
    } else {
      const auto& orig = itemsA[id];
      if (orig.startFrame != it.startFrame || orig.durationFrames != it.durationFrames ||
          orig.trackId != it.trackId || orig.sourceInFrame != it.sourceInFrame ||
          std::abs(orig.volume - it.volume) > 0.001 || std::abs(orig.speed - it.speed) > 0.001) {
        diff.modifiedItemIds.append(id);
      }
    }
  }

  for (const auto& [id, _] : itemsA) {
    if (itemsB.find(id) == itemsB.end()) {
      diff.removedItemIds.append(id);
    }
  }

  // Tracks diff
  std::set<QString> tracksA, tracksB;
  for (const auto& t : docA.tracks) tracksA.insert(t.id);
  for (const auto& t : docB.tracks) tracksB.insert(t.id);
  for (const auto& t : docB.tracks) {
    if (tracksA.find(t.id) == tracksA.end()) diff.trackChanges.append(QStringLiteral("+track:%1").arg(t.id));
  }
  for (const auto& t : docA.tracks) {
    if (tracksB.find(t.id) == tracksB.end()) diff.trackChanges.append(QStringLiteral("-track:%1").arg(t.id));
  }

  return diff;
}

bool BranchManager::mergeBranch(const QString& sourceBranch, const QString&) {
  const QString src = sourceBranch.trimmed();
  if (src.isEmpty() || src == activeBranch_) return false;

  auto it = branches_.find(src);
  if (it == branches_.end()) return false;

  if (!project_) return false;

  saveCurrentStateToActiveBranch();

  const TimelineDoc& srcDoc = it->second.doc;
  const TimelineDoc currDoc = project_->doc();

  project_->beginGroup(QStringLiteral("Merge branch %1").arg(src));

  // Determine items to add, remove, and update
  std::map<QString, Item> currItems, srcItems;
  for (const auto& item : currDoc.items) currItems[item.id] = item;
  for (const auto& item : srcDoc.items) srcItems[item.id] = item;

  std::vector<QString> toRemove;
  for (const auto& [id, _] : currItems) {
    if (srcItems.find(id) == srcItems.end()) toRemove.push_back(id);
  }
  if (!toRemove.empty()) {
    project_->apply(Op(ItemRemove{.itemIds = toRemove, .ripple = false}));
  }

  for (const auto& [id, item] : srcItems) {
    auto curIt = currItems.find(id);
    if (curIt == currItems.end()) {
      project_->apply(Op(ItemAdd{.item = item, .propsOmitted = false}));
    } else {
      const auto& cur = curIt->second;
      if (cur.startFrame != item.startFrame || cur.durationFrames != item.durationFrames ||
          cur.trackId != item.trackId || cur.sourceInFrame != item.sourceInFrame ||
          std::abs(cur.volume - item.volume) > 0.001) {
        ItemPatch patch;
        patch.trackId = item.trackId;
        patch.startFrame = item.startFrame;
        patch.durationFrames = item.durationFrames;
        patch.sourceInFrame = item.sourceInFrame;
        patch.volume = item.volume;
        project_->apply(Op(ItemUpdate{.itemId = id, .patch = patch}));
      }
    }
  }

  project_->endGroup();

  saveCurrentStateToActiveBranch();

  if (project_) {
    std::vector<BranchRecord> list;
    for (const auto& [_, b] : branches_) list.push_back(b);
    project_->setBranches(std::move(list));
  }

  emit branchMerged(src, activeBranch_);
  return true;
}

void BranchManager::syncToBundle(ProjectBundle& bundle) const {
  bundle.branches.clear();
  for (const auto& [_, rec] : branches_) {
    bundle.branches.push_back(rec);
  }
}

void BranchManager::loadFromBundle(const ProjectBundle& bundle) {
  branches_.clear();
  for (const auto& rec : bundle.branches) {
    branches_[rec.name] = rec;
  }
}

} // namespace sf::copilot
