#include "core/history.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

namespace sf {

void History::beginGroup(const std::optional<QString>& label, const std::optional<QString>& actor) {
  if (openGroup_) commitGroup();
  openGroup_ = UndoGroup{{}, {}, label, actor, std::nullopt};
}

void History::push(const std::vector<Op>& ops, const std::vector<Op>& inverses, const std::optional<QString>& label,
                   const std::optional<QString>& actor) {
  if (ops.empty()) return;
  if (openGroup_) {
    openGroup_->ops.insert(openGroup_->ops.end(), ops.begin(), ops.end());
    // undo applies a group's inverses in order, so later pushes must come first
    openGroup_->inverses.insert(openGroup_->inverses.begin(), inverses.begin(), inverses.end());
  } else {
    commit(UndoGroup{ops, inverses, label, actor, std::nullopt});
  }
}

std::optional<UndoGroup> History::commitGroup() {
  if (!openGroup_) return std::nullopt;
  UndoGroup group = std::move(*openGroup_);
  openGroup_.reset();
  if (group.ops.empty()) return std::nullopt;
  return commit(std::move(group));
}

UndoGroup History::commit(UndoGroup group) {
  // discard any redo tail
  stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(index_), stack_.end());
  if (!group.createdAt) group.createdAt = QDateTime::currentMSecsSinceEpoch();
  stack_.push_back(group);
  if (stack_.size() > limit_) stack_.pop_front();
  index_ = stack_.size();
  return group;
}

std::optional<UndoGroup> History::undoGroup() const {
  if (openGroup_) return openGroup_;
  if (index_ > 0) return stack_[index_ - 1];
  return std::nullopt;
}

std::optional<UndoGroup> History::undo() {
  if (openGroup_) commitGroup();
  if (index_ == 0) return std::nullopt;
  index_ -= 1;
  return stack_[index_];
}

std::optional<UndoGroup> History::redo() {
  if (index_ >= stack_.size()) return std::nullopt;
  const UndoGroup& group = stack_[index_];
  index_ += 1;
  return group;
}

void History::dropReferences(const QString& needle) {
  // what a persisted log would contain, like JSON.stringify(ops) on the TS side
  const auto serialised = [](const std::vector<Op>& ops) {
    QJsonArray a;
    for (const Op& op : ops) a.append(toJson(op));
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
  };
  const auto refs = [&](const UndoGroup& g) {
    return serialised(g.ops).contains(needle) || serialised(g.inverses).contains(needle);
  };
  if (openGroup_) commitGroup();
  std::ptrdiff_t lastUndo = -1;
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(index_) - 1; i >= 0; --i) {
    if (refs(stack_[static_cast<std::size_t>(i)])) {
      lastUndo = i;
      break;
    }
  }
  std::size_t firstRedo = stack_.size();
  for (std::size_t i = index_; i < stack_.size(); ++i) {
    if (refs(stack_[i])) {
      firstRedo = i;
      break;
    }
  }
  // keep stack[lastUndo + 1, firstRedo)
  stack_.erase(stack_.begin() + static_cast<std::ptrdiff_t>(firstRedo), stack_.end());
  stack_.erase(stack_.begin(), stack_.begin() + (lastUndo + 1));
  index_ -= static_cast<std::size_t>(lastUndo + 1);
}

void History::clear() {
  stack_.clear();
  index_ = 0;
  openGroup_.reset();
}

std::optional<QString> History::undoLabel() const {
  const auto g = undoGroup();
  return g ? g->label : std::nullopt;
}

} // namespace sf
