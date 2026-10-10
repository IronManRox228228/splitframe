#pragma once

#include "core/ops.h"

#include <deque>
#include <functional>
#include <optional>

namespace sf {

struct UndoGroup {
  // The ops that produced the change (redo re-applies them).
  std::vector<Op> ops;
  // Inverses captured at apply time; undo applies them in order.
  std::vector<Op> inverses;
  std::optional<QString> label;
  std::optional<QString> actor;
  std::optional<qint64> createdAt; // ms since the epoch
};

// Linear undo/redo history over committed op groups (packages/editor-core/src/history.ts). The
// owner (project service or UI model) applies ops/inverses to the doc; this only tracks the stack.
class History {
 public:
  explicit History(std::size_t limit = 500) : limit_(limit) {}

  // Begin coalescing subsequent pushes into one group (an agent turn, a drag).
  void beginGroup(const std::optional<QString>& label = {}, const std::optional<QString>& actor = {});

  // Record an applied change; if a group is open it joins that group.
  void push(const std::vector<Op>& ops, const std::vector<Op>& inverses, const std::optional<QString>& label = {},
            const std::optional<QString>& actor = {});

  // Close the open group (if any) as one undoable entry. Empty groups are dropped (nullopt).
  std::optional<UndoGroup> commitGroup();

  // The group undo would take: the open group while one is open, else the newest applied one.
  std::optional<UndoGroup> undoGroup() const;

  std::optional<UndoGroup> undo();
  std::optional<UndoGroup> redo();

  // Undo/redo one group by running `apply` on it. If `apply` throws (the doc changed under the
  // history) the position is put back and the exception propagates, so the stack and the doc never
  // drift apart. Returns nullopt when there is nothing to undo/redo.
  template <class F> auto undoWith(F&& apply) -> std::optional<decltype(apply(std::declval<const UndoGroup&>()))> {
    const auto group = undo();
    if (!group) return std::nullopt;
    try {
      return apply(*group);
    } catch (...) {
      index_ += 1;
      throw;
    }
  }
  template <class F> auto redoWith(F&& apply) -> std::optional<decltype(apply(std::declval<const UndoGroup&>()))> {
    const auto group = redo();
    if (!group) return std::nullopt;
    try {
      return apply(*group);
    } catch (...) {
      index_ -= 1;
      throw;
    }
  }

  // Committed groups, oldest first; the first position() of them are applied (read-only view for listings).
  const std::deque<UndoGroup>& groups() const { return stack_; }
  std::size_t position() const { return index_; }

  bool canUndo() const { return index_ > 0; }
  bool canRedo() const { return index_ < stack_.size(); }

  // Forget history that would bring back references to `needle` (an id that no longer exists,
  // e.g. a deleted asset). Undo drops the newest referencing group and everything older, since
  // those groups can no longer be reached without passing through it; redo drops the first
  // referencing group and everything after it. Groups in between stay valid.
  void dropReferences(const QString& needle);

  void clear();

  // For UI labels: "Split clip" etc.
  std::optional<QString> undoLabel() const;

 private:
  UndoGroup commit(UndoGroup group);

  std::deque<UndoGroup> stack_;
  std::size_t index_ = 0; // number of applied groups; stack_[index_] is the next redo
  std::size_t limit_;
  std::optional<UndoGroup> openGroup_;
};

} // namespace sf
