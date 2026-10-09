#include "core/timeline_doc.h"

#include "core/error.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>

namespace sf {

bool trackAllowsItem(TrackKind k, ItemType t) {
  switch (k) {
    case TrackKind::Video:
    case TrackKind::Overlay:
      return t == ItemType::Video || t == ItemType::Image || t == ItemType::Shape || t == ItemType::MotionGraphic;
    case TrackKind::Text:
      return t == ItemType::Text || t == ItemType::Caption || t == ItemType::Shape || t == ItemType::MotionGraphic;
    case TrackKind::Audio: return t == ItemType::Audio;
  }
  return false;
}

bool isMediaType(ItemType t) { return t == ItemType::Video || t == ItemType::Audio; }
bool isAudioBearing(const Item& item) { return isMediaType(item.type()); }
bool isVisual(const Item& item) { return item.type() != ItemType::Audio; }

const Track* getTrack(const TimelineDoc& doc, const QString& id) {
  const auto it = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& t) { return t.id == id; });
  return it == doc.tracks.end() ? nullptr : &*it;
}

const Item* getItem(const TimelineDoc& doc, const QString& id) {
  const auto it = std::find_if(doc.items.begin(), doc.items.end(), [&](const Item& i) { return i.id == id; });
  return it == doc.items.end() ? nullptr : &*it;
}

const Item& requireItem(const TimelineDoc& doc, const QString& id) {
  if (const Item* item = getItem(doc, id)) return *item;
  throw OpError(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call getTimeline to list current item ids."));
}

const Track& requireTrack(const TimelineDoc& doc, const QString& id) {
  if (const Track* track = getTrack(doc, id)) return *track;
  throw OpError(QStringLiteral("Track %1 not found.").arg(id), QStringLiteral("Call getTimeline to list current track ids."));
}

Frame itemEnd(const Item& item) { return item.startFrame + item.durationFrames; }

std::vector<const Item*> itemsOnTrack(const TimelineDoc& doc, const QString& trackId) {
  std::vector<const Item*> out;
  for (const Item& i : doc.items)
    if (i.trackId == trackId) out.push_back(&i);
  std::stable_sort(out.begin(), out.end(), [](const Item* a, const Item* b) {
    if (a->startFrame != b->startFrame) return a->startFrame < b->startFrame;
    return a->id < b->id;
  });
  return out;
}

Frame docDurationFrames(const TimelineDoc& doc) {
  Frame max = 0;
  for (const Item& i : doc.items) max = std::max(max, itemEnd(i));
  return max;
}

Frame sourceOutFrame(const Item& item) {
  if (!isMediaType(item.type()) || !item.sourceInFrame) return 0;
  return *item.sourceInFrame + jsRound(static_cast<double>(item.durationFrames) * item.speed);
}

Frame sourceFrameAt(const Item& item, Frame timelineFrame) {
  const double local = static_cast<double>(timelineFrame - item.startFrame);
  const double base = static_cast<double>(item.sourceInFrame.value_or(0));
  const auto clampRound = [](double v) { return std::max<Frame>(0, jsRound(v)); };
  if (!item.timeRemap.empty()) {
    std::vector<TimeRemapPoint> points = item.timeRemap;
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    const auto slope = [](const TimeRemapPoint& a, const TimeRemapPoint& b) {
      return static_cast<double>(b.sourceFrame - a.sourceFrame) / static_cast<double>(std::max<Frame>(1, b.frame - a.frame));
    };
    const TimeRemapPoint& first = points.front();
    if (local <= static_cast<double>(first.frame)) {
      // extrapolate linearly from the first segment (flat if there is a single point)
      if (points.size() == 1) return clampRound(base + local);
      return clampRound(static_cast<double>(first.sourceFrame) + (local - static_cast<double>(first.frame)) * slope(first, points[1]));
    }
    const TimeRemapPoint& last = points.back();
    if (local >= static_cast<double>(last.frame)) {
      if (points.size() == 1) return clampRound(static_cast<double>(last.sourceFrame) + (local - static_cast<double>(last.frame)));
      return clampRound(static_cast<double>(last.sourceFrame) +
                        (local - static_cast<double>(last.frame)) * slope(points[points.size() - 2], last));
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
      const TimeRemapPoint& a = points[i];
      const TimeRemapPoint& b = points[i + 1];
      if (local >= static_cast<double>(a.frame) && local <= static_cast<double>(b.frame)) {
        const double t = (local - static_cast<double>(a.frame)) / static_cast<double>(std::max<Frame>(1, b.frame - a.frame));
        return clampRound(static_cast<double>(a.sourceFrame) + t * static_cast<double>(b.sourceFrame - a.sourceFrame));
      }
    }
  }
  return clampRound(base + local * item.speed);
}

ItemProps defaultProps(ItemType type) { return parseItemProps(type, Rd(QJsonObject{}, QStringLiteral("props"))); }

namespace {

// Right alternative, arbitrary content: only used to carry the type through toJson before
// real props are merged in.
ItemProps placeholderProps(ItemType type) {
  switch (type) {
    case ItemType::Video: return VideoProps{};
    case ItemType::Audio: return AudioProps{};
    case ItemType::Image: return ImageProps{};
    case ItemType::Text: return TextProps{};
    case ItemType::Caption: return CaptionProps{};
    case ItemType::Shape: return ShapeProps{};
    case ItemType::MotionGraphic: return MotionGraphicProps{};
  }
  return ImageProps{};
}

} // namespace

Item createItem(ItemType type, const ItemInit& in) {
  Item base;
  base.id = in.id;
  base.trackId = in.trackId;
  base.startFrame = in.startFrame;
  base.durationFrames = in.durationFrames;
  base.assetId = in.assetId;
  base.sourceInFrame = in.sourceInFrame;
  base.speed = in.speed.value_or(1);
  base.timeRemap = in.timeRemap.value_or(std::vector<TimeRemapPoint>{});
  base.transform = Transform{};
  if (in.transform) {
    Transform& t = base.transform;
    const TransformPatch& p = *in.transform;
    if (p.x) t.x = *p.x;
    if (p.y) t.y = *p.y;
    if (p.scale) t.scale = *p.scale;
    if (p.scaleX) t.scaleX = *p.scaleX;
    if (p.scaleY) t.scaleY = *p.scaleY;
    if (p.rotation) t.rotation = *p.rotation;
    if (p.opacity) t.opacity = *p.opacity;
  }
  base.volume = in.volume.value_or(1);
  base.muted = in.muted.value_or(false);
  base.effects = in.effects.value_or(std::vector<Effect>{});
  base.masks = in.masks.value_or(std::vector<Mask>{});
  base.keyframes = in.keyframes.value_or(KeyframeMap{});
  base.labels = in.labels.value_or(Labels{});
  base.props = placeholderProps(type);

  // the schema parse is the validation: ranges, required props, defaults
  QJsonObject json = toJson(base, /*omitProps=*/true);
  json.insert(QStringLiteral("props"), in.props ? toJson(*in.props) : QJsonObject{});
  Item parsed = parseItem(Rd(json));
  if (isMediaType(type) && (!parsed.assetId || parsed.assetId->isEmpty()))
    throw OpError(QStringLiteral("%1 items require an assetId.").arg(enumName(type)),
                  QStringLiteral("Import the asset first and pass its assetId."));
  return parsed;
}

TimelineDoc createEmptyDoc(const EmptyDocInit& in) {
  const QString now = in.createdAt.value_or(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  const auto mkTrack = [](const QString& id, TrackKind kind, const QString& name) {
    Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    return t;
  };
  TimelineDoc d;
  d.project.id = in.id;
  d.project.name = in.name;
  d.project.fps = in.fps;
  d.project.width = in.width;
  d.project.height = in.height;
  d.project.createdAt = now;
  d.project.updatedAt = now;
  d.tracks = {mkTrack(in.id + QStringLiteral("_text"), TrackKind::Text, QStringLiteral("Text & Graphics")),
              mkTrack(in.id + QStringLiteral("_main"), TrackKind::Video, QStringLiteral("Main Video")),
              mkTrack(in.id + QStringLiteral("_audio"), TrackKind::Audio, QStringLiteral("Audio 1"))};
  // same validation the zod parse gives (fps/width/height must be positive integers)
  return parseTimelineDoc(Rd(toJson(d)));
}

TimelineDoc parseDoc(const QJsonValue& raw) { return parseTimelineDoc(Rd(raw)); }

} // namespace sf
