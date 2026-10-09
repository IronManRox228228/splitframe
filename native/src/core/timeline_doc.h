#pragma once

#include "core/schema.h"

// Lookups and constructors over a TimelineDoc (packages/editor-core/src/timeline-doc.ts).
namespace sf {

// Which item types a track kind may hold.
bool trackAllowsItem(TrackKind trackKind, ItemType itemType);
// video and audio carry source media (sourceInFrame, speed, assetId)
bool isMediaType(ItemType t);
bool isAudioBearing(const Item& item);
bool isVisual(const Item& item);

// Pointers stay valid until the doc is modified; null when absent.
const Track* getTrack(const TimelineDoc& doc, const QString& trackId);
const Item* getItem(const TimelineDoc& doc, const QString& itemId);
// Throw OpError with a hint when the id is unknown.
const Item& requireItem(const TimelineDoc& doc, const QString& itemId);
const Track& requireTrack(const TimelineDoc& doc, const QString& trackId);

Frame itemEnd(const Item& item);
// Sorted by start frame, then id (ids tie-break by code unit, not locale)
std::vector<const Item*> itemsOnTrack(const TimelineDoc& doc, const QString& trackId);
Frame docDurationFrames(const TimelineDoc& doc);

// Derived source out point (sourceIn + duration * speed); 0 for non-media items.
Frame sourceOutFrame(const Item& item);
// Timeline frame (absolute) to source-local frame, through the piecewise-linear time remap when
// present, otherwise constant speed.
Frame sourceFrameAt(const Item& item, Frame timelineFrame);

// Mirrors the TS createItem input: everything beyond the identity and placement is optional and
// takes its default.
struct ItemInit {
  QString id;
  QString trackId;
  Frame startFrame = 0;
  Frame durationFrames = 1;
  std::optional<QString> assetId;
  std::optional<Frame> sourceInFrame;
  std::optional<double> speed;
  std::optional<std::vector<TimeRemapPoint>> timeRemap;
  std::optional<TransformPatch> transform; // merged over DEFAULT_TRANSFORM
  std::optional<double> volume;
  std::optional<bool> muted;
  std::optional<std::vector<Effect>> effects;
  std::optional<std::vector<Mask>> masks;
  std::optional<KeyframeMap> keyframes;
  std::optional<Labels> labels;
  std::optional<ItemProps> props; // absent: schema defaults (throws for types with required fields)
};

// Validates against the schema (SchemaError) and requires an assetId for video/audio (OpError).
Item createItem(ItemType type, const ItemInit& init);

struct EmptyDocInit {
  QString id;
  QString name;
  std::int64_t fps = 30;
  std::int64_t width = 1920;
  std::int64_t height = 1080;
  std::optional<QString> createdAt; // default: now (UTC, ISO 8601)
};
// Text & Graphics, Main Video and Audio 1 tracks (ids "<id>_text", "<id>_main", "<id>_audio").
TimelineDoc createEmptyDoc(const EmptyDocInit& init);

// Validates a whole doc (SchemaError on the first bad field).
TimelineDoc parseDoc(const QJsonValue& raw);

// Zod defaults for item props of a type; throws SchemaError for types with required fields.
ItemProps defaultProps(ItemType type);

} // namespace sf
