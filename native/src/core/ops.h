#pragma once

#include "core/schema.h"

#include <type_traits>

// Ops are the only way timeline state changes (packages/schema/src/ops.ts). They carry every id
// they create, so replaying an op log is deterministic. Optional fields that are absent mean
// "not specified", as with zod's optional().
namespace sf {

struct ProjectRename { QString name; };
struct ProjectSetCanvas { Frame width = 0; Frame height = 0; };
// Rescales every item, keyframe and marker so each keeps its time in seconds.
struct ProjectSetFps { std::int64_t fps = 30; };
// Internal: the exact inverse of ProjectSetFps.
struct ProjectRestoreTimeline {
  std::int64_t fps = 30;
  std::vector<Item> items;
  std::vector<Marker> markers;
};
struct ProjectSetStyleConfig { StyleConfig styleConfig; };
// Reference video for style matching; null (nullopt) clears it.
struct ProjectSetReference { std::optional<QString> assetId; };

struct TrackAdd {
  QString trackId;
  TrackKind kind = TrackKind::Video;
  QString name;
  bool locked = false;
  bool muted = false;
  bool hidden = false;
  std::optional<std::int64_t> index; // 0 = top; default: appended at the bottom
};
struct TrackRemove { QString trackId; };
struct TrackPatch {
  std::optional<QString> name;
  std::optional<bool> locked, muted, hidden;
};
struct TrackUpdate {
  QString trackId;
  TrackPatch patch;
};
struct TrackReorder { std::vector<QString> trackIds; };

// item.add may omit type-specific props (`propsOmitted`); apply fills the schema defaults.
struct ItemAdd {
  Item item;
  bool propsOmitted = false;
};
struct ItemRemove {
  std::vector<QString> itemIds;
  bool ripple = false;
};
struct ItemUpdate {
  QString itemId;
  ItemPatch patch;
};
struct ItemMove {
  QString itemId;
  std::optional<QString> trackId;
  std::optional<Frame> startFrame;
};
struct ItemTrim {
  QString itemId;
  TrimEdge edge = TrimEdge::In;
  Frame frame = 0;
  bool ripple = false;
};
struct ItemSplit {
  QString itemId;
  Frame atFrame = 0;
  QString newItemId;
};
struct ItemClone {
  QString itemId;
  QString newItemId;
  std::optional<QString> trackId;
  std::optional<Frame> startFrame;
};
struct ItemSlip {
  QString itemId;
  Frame sourceInFrame = 0;
};
struct ItemSetSpeed {
  QString itemId;
  double speed = 1;
};
struct ItemSetTimeRemap {
  QString itemId;
  std::vector<TimeRemapPoint> points;
};
struct ItemSetKeyframes {
  QString itemId;
  QString property;
  KeyframeList keyframes;
};

struct EffectAdd {
  QString itemId;
  Effect effect;
};
struct EffectRemove {
  QString itemId;
  QString effectId;
};
struct EffectPatch {
  std::optional<QString> type;
  std::optional<EffectParams> params;
};
struct EffectUpdate {
  QString itemId;
  QString effectId;
  EffectPatch patch;
};

struct MaskAdd {
  QString itemId;
  Mask mask;
};
struct MaskRemove {
  QString itemId;
  QString maskId;
};

struct MarkerAdd { Marker marker; };
struct MarkerRemove { QString markerId; };
struct MarkerPatch {
  std::optional<Frame> frame;
  std::optional<QString> label;
  std::optional<QString> color;
};
struct MarkerUpdate {
  QString markerId;
  MarkerPatch patch;
};

struct Op;
// Atomic: all ops apply or none. Nested batches are rejected at apply time (they still parse).
struct BatchOp { std::vector<Op> ops; };

using OpBody = std::variant<ProjectRename, ProjectSetCanvas, ProjectSetFps, ProjectRestoreTimeline,
                            ProjectSetStyleConfig, ProjectSetReference, TrackAdd, TrackRemove, TrackUpdate,
                            TrackReorder, ItemAdd, ItemRemove, ItemUpdate, ItemMove, ItemTrim, ItemSplit,
                            ItemClone, ItemSlip, ItemSetSpeed, ItemSetTimeRemap, ItemSetKeyframes, EffectAdd,
                            EffectRemove, EffectUpdate, MaskAdd, MaskRemove, MarkerAdd, MarkerRemove,
                            MarkerUpdate, BatchOp>;

struct Op {
  OpBody body;

  Op() = default;
  template <class T>
    requires(!std::is_same_v<std::decay_t<T>, Op> && std::is_constructible_v<OpBody, T &&>)
  Op(T&& value) : body(std::forward<T>(value)) {} // NOLINT: ops convert implicitly, like TS literals

  // "item.move", "batch", ... : the discriminator used in JSON
  QString type() const;
};

// Compares serialised forms: what a persisted op log would see (explicit-undefined fields in
// inverse patches are not part of the identity, same as after JSON.stringify).
bool operator==(const Op& a, const Op& b);

Op parseOp(const Rd& r);
QJsonObject toJson(const Op& op);
// Throws SchemaError if `op` violates a schema constraint (empty name, speed out of range, ...).
// Typed ops are built in code, so this is what stands in for opSchema.parse on the TS side.
void validateOp(const Op& op);

// One committed change plus who made it.
struct OpEntry {
  std::int64_t seq = 0;
  QString actor; // 'user' | 'builtin-agent' | `mcp:${string}`
  Op op;
  QString createdAt;
};
OpEntry makeOpEntry(const Op& op, std::int64_t seq, const QString& actor);
OpEntry parseOpEntry(const Rd& r);
QJsonObject toJson(const OpEntry& e);

} // namespace sf
