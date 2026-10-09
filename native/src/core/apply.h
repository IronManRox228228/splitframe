#pragma once

#include "core/ops.h"
#include "core/timeline_doc.h"

// The apply engine (packages/editor-core/src/apply.ts): ops in, new doc plus inverse ops out.
namespace sf {

struct ApplyResult {
  TimelineDoc doc;
  // Ops that undo this op, computed against the pre-op doc. Apply in order.
  std::vector<Op> inverse;
};

struct ApplyOptions {
  // Reject edits to items on locked tracks. Undo/redo replay recorded inverses with this off, so
  // a track locked after an edit can still be undone.
  bool enforceLocks = true;
};

// Pure: the input doc is never modified. Atomic: on error (SchemaError for a malformed op,
// OpError for one that cannot apply here) nothing changes.
ApplyResult applyOp(const TimelineDoc& doc, const Op& op, ApplyOptions opts = {});

// Applies in order; the inverses come back flattened newest-first so they undo the whole list.
ApplyResult applyOps(const TimelineDoc& doc, const std::vector<Op>& ops, ApplyOptions opts = {});

// Frames per source frame: the overall slope of the remap when there is one, else `speed`.
double effectiveSpeed(const Item& item);

} // namespace sf
