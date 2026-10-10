#include "core/apply.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace sf {

namespace {

using Inverse = std::vector<Op>;

template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

QString num(Frame v) { return QString::number(static_cast<qint64>(v)); }

Item& itemRef(TimelineDoc& d, const QString& id) {
  return *std::find_if(d.items.begin(), d.items.end(), [&](const Item& i) { return i.id == id; });
}

bool contains(const std::vector<QString>& v, const QString& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// Change fps in place: every frame count keeps its time in seconds (boundaries are rounded, so
// neighbours stay butted).
void rescaleFps(TimelineDoc& d, std::int64_t fps) {
  const double r = static_cast<double>(fps) / static_cast<double>(d.project.fps);
  const auto f = [r](Frame n) { return jsRound(static_cast<double>(n) * r); };
  for (Item& it : d.items) {
    const Frame end = f(it.startFrame + it.durationFrames);
    it.startFrame = f(it.startFrame);
    it.durationFrames = std::max<Frame>(1, end - it.startFrame);
    if (it.sourceInFrame) it.sourceInFrame = f(*it.sourceInFrame);
    for (auto& [prop, kfs] : it.keyframes)
      for (Keyframe& k : kfs) k.frame = f(k.frame);
    for (TimeRemapPoint& p : it.timeRemap) {
      p.frame = f(p.frame);
      p.sourceFrame = f(p.sourceFrame);
    }
    std::visit(
        [&](auto& props) {
          if constexpr (std::is_base_of_v<FadeProps, std::decay_t<decltype(props)>>) {
            if (props.fadeInFrames) props.fadeInFrames = f(props.fadeInFrames);
            if (props.fadeOutFrames) props.fadeOutFrames = f(props.fadeOutFrames);
          }
        },
        it.props);
  }
  for (Marker& m : d.markers) m.frame = f(m.frame);
  d.project.fps = fps;
}

/// Tracks an op would modify: its items' tracks and any destination track.
void assertTracksUnlocked(const TimelineDoc& doc, const Op& op) {
  std::vector<QString> trackIds;
  const auto ofItem = [&](const QString& itemId) {
    if (const Item* item = getItem(doc, itemId)) trackIds.push_back(item->trackId);
  };
  const bool relevant = std::visit(
      Overloaded{
          [&](const ItemAdd& o) { trackIds.push_back(o.item.trackId); return true; },
          [&](const ItemRemove& o) { for (const QString& id : o.itemIds) ofItem(id); return true; },
          [&](const ItemUpdate& o) {
            ofItem(o.itemId);
            if (o.patch.trackId && !o.patch.trackId->isEmpty()) trackIds.push_back(*o.patch.trackId);
            return true;
          },
          [&](const ItemMove& o) {
            ofItem(o.itemId);
            if (o.trackId && !o.trackId->isEmpty()) trackIds.push_back(*o.trackId);
            return true;
          },
          [&](const ItemClone& o) {
            // reading from a locked track is fine; the copy lands on the destination
            const Item* src = getItem(doc, o.itemId);
            trackIds.push_back(o.trackId && !o.trackId->isEmpty() ? *o.trackId : (src ? src->trackId : QString()));
            return true;
          },
          [&](const TrackRemove& o) { trackIds.push_back(o.trackId); return true; },
          [&](const ItemTrim& o) { ofItem(o.itemId); return true; },
          [&](const ItemSplit& o) { ofItem(o.itemId); return true; },
          [&](const ItemSlip& o) { ofItem(o.itemId); return true; },
          [&](const ItemSetSpeed& o) { ofItem(o.itemId); return true; },
          [&](const ItemSetTimeRemap& o) { ofItem(o.itemId); return true; },
          [&](const ItemSetKeyframes& o) { ofItem(o.itemId); return true; },
          [&](const EffectAdd& o) { ofItem(o.itemId); return true; },
          [&](const EffectRemove& o) { ofItem(o.itemId); return true; },
          [&](const EffectUpdate& o) { ofItem(o.itemId); return true; },
          [&](const MaskAdd& o) { ofItem(o.itemId); return true; },
          [&](const MaskRemove& o) { ofItem(o.itemId); return true; },
          [](const auto&) { return false; },
      },
      op.body);
  if (!relevant) return;
  for (const QString& id : trackIds) {
    const Track* track = getTrack(doc, id);
    if (track && track->locked)
      throw OpError(QStringLiteral("Track \"%1\" is locked.").arg(track->name), QStringLiteral("Unlock the track first."));
  }
}

// ---------- project ----------

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectRename& o) {
  next.project.name = o.name;
  return {ProjectRename{doc.project.name}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectSetCanvas& o) {
  next.project.width = o.width;
  next.project.height = o.height;
  return {ProjectSetCanvas{doc.project.width, doc.project.height}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectSetFps& o) {
  Inverse inverse{ProjectRestoreTimeline{doc.project.fps, doc.items, doc.markers}};
  rescaleFps(next, o.fps);
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectRestoreTimeline& o) {
  Inverse inverse{ProjectRestoreTimeline{doc.project.fps, doc.items, doc.markers}};
  next.project.fps = o.fps;
  next.items = o.items;
  next.markers = o.markers;
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectSetStyleConfig& o) {
  next.project.styleConfig = o.styleConfig;
  return {ProjectSetStyleConfig{doc.project.styleConfig}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ProjectSetReference& o) {
  // references are metadata, not timeline items: the tool layer validates them against assets
  next.project.referenceAssetId = o.assetId;
  return {ProjectSetReference{doc.project.referenceAssetId}};
}

// ---------- tracks ----------

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const TrackAdd& o) {
  if (getTrack(doc, o.trackId)) throw OpError(QStringLiteral("Track %1 already exists.").arg(o.trackId));
  Track track;
  track.id = o.trackId;
  track.kind = o.kind;
  track.name = o.name;
  track.locked = o.locked;
  track.muted = o.muted;
  track.hidden = o.hidden;
  const auto index = static_cast<std::size_t>(std::min<std::int64_t>(o.index.value_or(static_cast<std::int64_t>(doc.tracks.size())),
                                                                     static_cast<std::int64_t>(next.tracks.size())));
  next.tracks.insert(next.tracks.begin() + static_cast<std::ptrdiff_t>(index), track);
  return {TrackRemove{o.trackId}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const TrackRemove& o) {
  const auto it = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& t) { return t.id == o.trackId; });
  if (it == doc.tracks.end()) throw OpError(QStringLiteral("Track %1 not found.").arg(o.trackId));
  const Track track = *it;
  const auto index = it - doc.tracks.begin();
  next.tracks.erase(next.tracks.begin() + index);
  std::erase_if(next.items, [&](const Item& i) { return i.trackId == o.trackId; });
  // restore the track at its old index, then the items
  TrackAdd add;
  add.trackId = track.id;
  add.kind = track.kind;
  add.name = track.name;
  add.locked = track.locked;
  add.muted = track.muted;
  add.hidden = track.hidden;
  add.index = static_cast<std::int64_t>(index);
  Inverse inverse{add};
  for (const Item& i : doc.items)
    if (i.trackId == o.trackId) inverse.emplace_back(ItemAdd{i, false});
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const TrackUpdate& o) {
  const Track* track = getTrack(doc, o.trackId);
  if (!track) throw OpError(QStringLiteral("Track %1 not found.").arg(o.trackId));
  TrackUpdate inv;
  inv.trackId = o.trackId;
  if (o.patch.name) inv.patch.name = track->name;
  if (o.patch.locked) inv.patch.locked = track->locked;
  if (o.patch.muted) inv.patch.muted = track->muted;
  if (o.patch.hidden) inv.patch.hidden = track->hidden;
  Track& t = *std::find_if(next.tracks.begin(), next.tracks.end(), [&](const Track& x) { return x.id == o.trackId; });
  if (o.patch.name) t.name = *o.patch.name;
  if (o.patch.locked) t.locked = *o.patch.locked;
  if (o.patch.muted) t.muted = *o.patch.muted;
  if (o.patch.hidden) t.hidden = *o.patch.hidden;
  return {inv};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const TrackReorder& o) {
  const auto key = [](std::vector<QString> ids) {
    std::sort(ids.begin(), ids.end());
    return ids.empty() ? QString() : QStringList(ids.begin(), ids.end()).join(QLatin1Char(','));
  };
  std::vector<QString> current;
  for (const Track& t : doc.tracks) current.push_back(t.id);
  if (key(current) != key(o.trackIds))
    throw OpError(QStringLiteral("track.reorder must contain exactly the existing track ids."),
                  QStringLiteral("Call getTimeline to read the current order first."));
  next.tracks.clear();
  for (const QString& id : o.trackIds) next.tracks.push_back(*getTrack(doc, id));
  return {TrackReorder{current}};
}

// ---------- items ----------

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemAdd& o) {
  const Track& track = requireTrack(doc, o.item.trackId);
  if (getItem(doc, o.item.id)) throw OpError(QStringLiteral("Item %1 already exists.").arg(o.item.id));
  const ItemType type = o.item.type();
  if (!trackAllowsItem(track.kind, type))
    throw OpError(QStringLiteral("A %1 item cannot live on the \"%2\" track \"%3\".").arg(enumName(type), enumName(track.kind), track.name),
                  QStringLiteral("Allowed item types on a %1 track: see the track-kind rules.").arg(enumName(track.kind)));
  // type-specific props may be omitted by tools; fill from the schema defaults
  Item item = o.item;
  if (o.propsOmitted) item.props = defaultProps(type);
  next.items.push_back(item);
  return {ItemRemove{{item.id}, false}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemRemove& o) {
  std::vector<QString> missing;
  for (const QString& id : o.itemIds)
    if (!getItem(doc, id)) missing.push_back(id);
  if (!missing.empty()) throw OpError(QStringLiteral("Items not found: %1.").arg(QStringList(missing.begin(), missing.end()).join(QStringLiteral(", "))));
  std::vector<const Item*> removed;
  for (const Item& i : doc.items)
    if (contains(o.itemIds, i.id)) removed.push_back(&i);

  // ripple: per track, close the gap left by the removed items (insertion-ordered like a JS Map)
  std::vector<std::pair<QString, Frame>> shifts;
  if (o.ripple) {
    std::vector<std::pair<QString, std::vector<const Item*>>> byTrack;
    for (const Item* item : removed) {
      auto it = std::find_if(byTrack.begin(), byTrack.end(), [&](const auto& p) { return p.first == item->trackId; });
      if (it == byTrack.end()) it = byTrack.insert(byTrack.end(), {item->trackId, {}});
      it->second.push_back(item);
    }
    for (const auto& [trackId, removedOnTrack] : byTrack) {
      Frame gap = 0;
      Frame minStart = std::numeric_limits<Frame>::max();
      Frame maxEnd = std::numeric_limits<Frame>::min();
      for (const Item* i : removedOnTrack) {
        gap += i->durationFrames;
        minStart = std::min(minStart, i->startFrame);
        maxEnd = std::max(maxEnd, itemEnd(*i));
      }
      for (const Item& item : doc.items) {
        if (item.trackId != trackId || contains(o.itemIds, item.id)) continue;
        if (item.startFrame >= maxEnd) shifts.emplace_back(item.id, -gap);
        else if (item.startFrame >= minStart) shifts.emplace_back(item.id, minStart - item.startFrame); // inside the removed span
      }
    }
  }

  std::erase_if(next.items, [&](const Item& i) { return contains(o.itemIds, i.id); });
  for (Item& item : next.items) {
    const auto s = std::find_if(shifts.begin(), shifts.end(), [&](const auto& p) { return p.first == item.id; });
    if (s != shifts.end()) item.startFrame = std::max<Frame>(0, item.startFrame + s->second);
  }

  Inverse inverse;
  for (const Item* i : removed) inverse.emplace_back(ItemAdd{*i, false});
  for (const auto& [itemId, delta] : shifts) {
    ItemUpdate u;
    u.itemId = itemId;
    u.patch.startFrame = getItem(doc, itemId)->startFrame;
    inverse.emplace_back(std::move(u));
  }
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemUpdate& o) {
  const Item& item = requireItem(doc, o.itemId);
  const ItemPatch& patch = o.patch;

  if (patch.trackId && *patch.trackId != item.trackId) {
    const Track& track = requireTrack(doc, *patch.trackId);
    if (!trackAllowsItem(track.kind, item.type()))
      throw OpError(QStringLiteral("A %1 item cannot move to the \"%2\" track \"%3\".").arg(enumName(item.type()), enumName(track.kind), track.name));
  }
  // whole-replace; shape-check against the item's own type
  std::optional<ItemProps> newProps;
  if (patch.props) newProps = parseItemProps(item.type(), Rd(*patch.props, QStringLiteral("patch.props")));

  ItemPatch inv;
  if (patch.trackId) inv.trackId = item.trackId;
  if (patch.startFrame) inv.startFrame = item.startFrame;
  if (patch.durationFrames) inv.durationFrames = item.durationFrames;
  if (patch.sourceInFrame) inv.sourceInFrame = item.sourceInFrame; // empty when there was none: nothing to restore
  if (patch.speed) inv.speed = item.speed;
  if (patch.timeRemap) inv.timeRemap = item.timeRemap;
  if (patch.transform) {
    TransformPatch t;
    const TransformPatch& p = *patch.transform;
    if (p.x) t.x = item.transform.x;
    if (p.y) t.y = item.transform.y;
    if (p.scale) t.scale = item.transform.scale;
    if (p.scaleX) t.scaleX = item.transform.scaleX;
    if (p.scaleY) t.scaleY = item.transform.scaleY;
    if (p.rotation) t.rotation = item.transform.rotation;
    if (p.opacity) t.opacity = item.transform.opacity;
    inv.transform = t;
  }
  if (patch.volume) inv.volume = item.volume;
  if (patch.muted) inv.muted = item.muted;
  if (patch.effects) inv.effects = item.effects;
  if (patch.masks) inv.masks = item.masks;
  if (patch.keyframes) {
    KeyframeMap kf;
    for (const auto& [prop, list] : *patch.keyframes) {
      const auto it = item.keyframes.find(prop);
      kf[prop] = it == item.keyframes.end() ? KeyframeList{} : it->second;
    }
    inv.keyframes = kf;
  }
  if (patch.labels) {
    // labels merge, so the inverse names every touched key (empty = remove it again)
    LabelsPatch l;
    if (patch.labels->name) l.name = item.labels.name;
    if (patch.labels->color) l.color = item.labels.color;
    inv.labels = l;
  }
  if (patch.color) inv.color = item.color.value_or(ItemColor{});
  if (patch.props) inv.props = QJsonValue(toJson(item.props));

  Item& t = itemRef(next, o.itemId);
  if (patch.trackId) t.trackId = *patch.trackId;
  if (patch.startFrame) t.startFrame = std::max<Frame>(0, *patch.startFrame);
  if (patch.durationFrames) t.durationFrames = std::max<Frame>(1, *patch.durationFrames);
  if (patch.sourceInFrame) t.sourceInFrame = std::max<Frame>(0, *patch.sourceInFrame);
  if (patch.speed) t.speed = *patch.speed;
  if (patch.timeRemap) t.timeRemap = *patch.timeRemap;
  if (patch.transform) {
    const TransformPatch& p = *patch.transform;
    if (p.x) t.transform.x = *p.x;
    if (p.y) t.transform.y = *p.y;
    if (p.scale) t.transform.scale = *p.scale;
    if (p.scaleX) t.transform.scaleX = *p.scaleX;
    if (p.scaleY) t.transform.scaleY = *p.scaleY;
    if (p.rotation) t.transform.rotation = *p.rotation;
    if (p.opacity) t.transform.opacity = *p.opacity;
  }
  if (patch.volume) t.volume = *patch.volume;
  if (patch.muted) t.muted = *patch.muted;
  if (patch.effects) t.effects = *patch.effects;
  if (patch.masks) t.masks = *patch.masks;
  if (patch.keyframes) {
    for (const auto& [prop, kfs] : *patch.keyframes) {
      if (kfs.empty()) t.keyframes.erase(prop);
      else t.keyframes[prop] = kfs;
    }
  }
  if (patch.labels) {
    if (patch.labels->name) t.labels.name = *patch.labels->name;
    if (patch.labels->color) t.labels.color = *patch.labels->color;
  }
  if (patch.color) {
    if (*patch.color == ItemColor{}) t.color.reset();
    else t.color = *patch.color;
  }
  if (newProps) t.props = *newProps;
  return {ItemUpdate{o.itemId, inv}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemMove& o) {
  const Item& item = requireItem(doc, o.itemId);
  const QString oldTrackId = item.trackId;
  const Frame oldStart = item.startFrame;
  if (o.trackId && *o.trackId != oldTrackId) {
    const Track& track = requireTrack(doc, *o.trackId);
    if (!trackAllowsItem(track.kind, item.type()))
      throw OpError(QStringLiteral("A %1 item cannot move to the \"%2\" track \"%3\".").arg(enumName(item.type()), enumName(track.kind), track.name));
  }
  if (!o.trackId && !o.startFrame) throw OpError(QStringLiteral("item.move needs a trackId or startFrame."));
  Inverse inverse;
  if (o.trackId) inverse.emplace_back(ItemMove{item.id, oldTrackId, std::nullopt});
  if (o.startFrame) inverse.emplace_back(ItemMove{item.id, std::nullopt, oldStart});
  Item& t = itemRef(next, o.itemId);
  if (o.trackId) t.trackId = *o.trackId;
  if (o.startFrame) t.startFrame = std::max<Frame>(0, *o.startFrame);
  return inverse;
}

struct Trim {
  Frame oldStart, oldEnd, oldSourceIn, newStart, newDuration, newSourceIn, downstreamDelta;
  bool isMedia;
};

// Shared trim logic. Plain trim keeps the opposite edge fixed; ripple trim anchors the item's
// start (for `in`) and slides downstream items by the delta so the timeline length changes by
// exactly the trim amount. Clamping is done in doubles like the TS, which also has +/-Infinity.
Trim computeTrim(const TimelineDoc& doc, const ItemTrim& o) {
  const Item& item = requireItem(doc, o.itemId);
  const Track& track = requireTrack(doc, item.trackId);
  if (track.locked)
    throw OpError(QStringLiteral("Track \"%1\" is locked.").arg(track.name), QStringLiteral("Unlock the track first."));
  Trim t{};
  t.oldStart = item.startFrame;
  t.oldEnd = itemEnd(item);
  t.isMedia = isMediaType(item.type());
  t.oldSourceIn = item.sourceInFrame.value_or(0);
  t.newStart = t.oldStart;
  t.newDuration = item.durationFrames;
  t.newSourceIn = t.oldSourceIn;
  t.downstreamDelta = 0;

  if (o.edge == TrimEdge::In) {
    const double delta = static_cast<double>(o.frame - t.oldStart); // >0: shorten head, <0: extend head
    // Furthest the head may extend: keep sourceIn >= 0 for media and, when the start moves
    // (plain trim), keep the start >= 0. Furthest it may shorten: leave one frame.
    constexpr double inf = std::numeric_limits<double>::infinity();
    const double sourceLimit = t.isMedia && item.speed > 0 ? std::ceil(-static_cast<double>(t.oldSourceIn) / item.speed) : -inf;
    const double minDelta = std::max(sourceLimit, o.ripple ? -inf : -static_cast<double>(t.oldStart));
    const double clamped = std::min(static_cast<double>(item.durationFrames - 1), std::max(delta, minDelta));
    if (t.isMedia) t.newSourceIn = t.oldSourceIn + jsRound(clamped * item.speed);
    const auto clampedFrames = static_cast<Frame>(clamped);
    if (o.ripple) {
      t.newStart = t.oldStart;
      t.newDuration = t.oldEnd - (t.oldStart + clampedFrames);
      t.downstreamDelta = -clampedFrames;
    } else {
      t.newStart = t.oldStart + clampedFrames;
      t.newDuration = t.oldEnd - t.newStart;
    }
  } else {
    const Frame newEnd = std::max(o.frame, t.oldStart + 1);
    t.newDuration = newEnd - t.oldStart;
    if (o.ripple) t.downstreamDelta = newEnd - t.oldEnd;
  }

  if (t.newStart == t.oldStart && t.newDuration == item.durationFrames && t.downstreamDelta == 0)
    throw OpError(QStringLiteral("Trim would not change the item."), QStringLiteral("Choose a frame outside the current edges."));
  return t;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemTrim& o) {
  const Trim t = computeTrim(doc, o);
  const Item& item = requireItem(doc, o.itemId);

  for (Item& i : next.items) {
    if (i.id == item.id) {
      i.startFrame = t.newStart;
      i.durationFrames = t.newDuration;
      if (t.isMedia) i.sourceInFrame = t.newSourceIn;
    } else if (t.downstreamDelta != 0 && i.trackId == item.trackId && i.startFrame >= t.oldEnd) {
      i.startFrame = std::max<Frame>(0, i.startFrame + t.downstreamDelta);
    }
  }

  // Restore the old geometry directly: re-running a trim would re-derive sourceIn from a
  // rounded value and could land a frame off at fractional speeds.
  ItemUpdate restore;
  restore.itemId = o.itemId;
  restore.patch.startFrame = t.oldStart;
  restore.patch.durationFrames = item.durationFrames;
  if (t.isMedia && t.newSourceIn != t.oldSourceIn) restore.patch.sourceInFrame = t.oldSourceIn;
  Inverse inverse{restore};
  if (t.downstreamDelta != 0) {
    for (const Item& i : doc.items) {
      if (i.trackId == item.trackId && i.id != o.itemId && i.startFrame >= t.oldEnd) {
        ItemUpdate u;
        u.itemId = i.id;
        u.patch.startFrame = i.startFrame;
        inverse.emplace_back(std::move(u));
      }
    }
  }
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemSplit& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (o.atFrame <= item.startFrame || o.atFrame >= itemEnd(item))
    throw OpError(QStringLiteral("Split frame %1 is outside item %2 (%3–%4).").arg(num(o.atFrame), item.id, num(item.startFrame), num(itemEnd(item))),
                  QStringLiteral("Pick a frame strictly inside the item."));
  const Frame splitLocal = o.atFrame - item.startFrame;
  Item a = item;
  a.durationFrames = splitLocal;
  Item b = item;
  b.id = o.newItemId;
  b.startFrame = o.atFrame;
  b.durationFrames = item.durationFrames - splitLocal;
  if (isMediaType(item.type())) b.sourceInFrame = item.sourceInFrame.value_or(0) + jsRound(static_cast<double>(splitLocal) * item.speed);
  b.timeRemap.clear();
  a.timeRemap.clear();
  for (const TimeRemapPoint& p : item.timeRemap) {
    if (p.frame >= splitLocal) b.timeRemap.push_back({p.frame - splitLocal, p.sourceFrame});
    if (p.frame < splitLocal) a.timeRemap.push_back(p);
  }
  // caption word times are relative to the item start: re-base the second half
  const bool splitsCaption = item.type() == ItemType::Caption;
  if (splitsCaption) {
    const Ms cutMs = jsRound(static_cast<double>(splitLocal) / static_cast<double>(doc.project.fps) * 1000.0);
    auto& pa = std::get<CaptionProps>(a.props);
    auto& pb = std::get<CaptionProps>(b.props);
    std::erase_if(pa.words, [&](const TranscriptWord& w) { return !(w.startMs < cutMs); });
    std::erase_if(pb.words, [&](const TranscriptWord& w) { return !(w.endMs > cutMs); });
    for (TranscriptWord& w : pb.words) {
      w.startMs -= cutMs;
      w.endMs -= cutMs;
    }
  }
  // split keyframe lists at the cut
  a.keyframes.clear();
  b.keyframes.clear();
  for (const auto& [prop, kfs] : item.keyframes) {
    KeyframeList& ka = a.keyframes[prop];
    KeyframeList& kb = b.keyframes[prop];
    for (const Keyframe& k : kfs) {
      if (k.frame < splitLocal) ka.push_back(k);
      if (k.frame >= splitLocal) kb.push_back({k.frame - splitLocal, k.value, k.easing});
    }
  }

  ItemUpdate restore;
  restore.itemId = item.id;
  restore.patch.durationFrames = item.durationFrames;
  restore.patch.timeRemap = item.timeRemap;
  restore.patch.keyframes = item.keyframes;
  if (splitsCaption) restore.patch.props = QJsonValue(toJson(item.props));
  Inverse inverse{ItemRemove{{o.newItemId}, false}, restore};

  *std::find_if(next.items.begin(), next.items.end(), [&](const Item& i) { return i.id == item.id; }) = a;
  next.items.push_back(std::move(b));
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemClone& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (getItem(doc, o.newItemId)) throw OpError(QStringLiteral("Item %1 already exists.").arg(o.newItemId));
  if (o.trackId && *o.trackId != item.trackId) {
    const Track& track = requireTrack(doc, *o.trackId);
    if (!trackAllowsItem(track.kind, item.type()))
      throw OpError(QStringLiteral("A %1 item cannot live on the \"%2\" track \"%3\".").arg(enumName(item.type()), enumName(track.kind), track.name));
  }
  Item copy = item;
  copy.id = o.newItemId;
  copy.trackId = o.trackId.value_or(item.trackId);
  copy.startFrame = o.startFrame.value_or(itemEnd(item));
  next.items.push_back(std::move(copy));
  return {ItemRemove{{o.newItemId}, false}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemSlip& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (!isAudioBearing(item))
    throw OpError(QStringLiteral("Only media items can slip (item %1 is %2).").arg(item.id, enumName(item.type())));
  itemRef(next, o.itemId).sourceInFrame = std::max<Frame>(0, o.sourceInFrame);
  return {ItemSlip{o.itemId, item.sourceInFrame.value_or(0)}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemSetSpeed& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (!isMediaType(item.type()))
    throw OpError(QStringLiteral("Only media items can be retimed (item %1 is %2).").arg(item.id, enumName(item.type())));
  const bool hadRemap = !item.timeRemap.empty();
  Inverse inverse{ItemSetSpeed{item.id, item.speed}};
  if (hadRemap) inverse.emplace_back(ItemSetTimeRemap{item.id, item.timeRemap});
  Item& t = itemRef(next, o.itemId);
  t.speed = o.speed;
  if (hadRemap) t.timeRemap.clear();
  return inverse;
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemSetTimeRemap& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (!isMediaType(item.type()))
    throw OpError(QStringLiteral("Time remap applies to media items only (item %1 is %2).").arg(item.id, enumName(item.type())));
  std::vector<TimeRemapPoint> points = o.points;
  std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
  for (const TimeRemapPoint& p : points) {
    if (p.frame < 0 || p.frame > item.durationFrames)
      throw OpError(QStringLiteral("Time remap point at frame %1 is outside the item (0–%2).").arg(num(p.frame), num(item.durationFrames)),
                    QStringLiteral("Use item-local frames."));
  }
  itemRef(next, o.itemId).timeRemap = std::move(points);
  return {ItemSetTimeRemap{o.itemId, item.timeRemap}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const ItemSetKeyframes& o) {
  const Item& item = requireItem(doc, o.itemId);
  KeyframeList kfs = o.keyframes;
  std::stable_sort(kfs.begin(), kfs.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
  const auto old = item.keyframes.find(o.property);
  KeyframeList before = old == item.keyframes.end() ? KeyframeList{} : old->second;
  Item& t = itemRef(next, o.itemId);
  if (kfs.empty()) t.keyframes.erase(o.property);
  else t.keyframes[o.property] = std::move(kfs);
  return {ItemSetKeyframes{o.itemId, o.property, std::move(before)}};
}

// ---------- effects and masks ----------

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const EffectAdd& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (std::any_of(item.effects.begin(), item.effects.end(), [&](const Effect& e) { return e.id == o.effect.id; }))
    throw OpError(QStringLiteral("Effect %1 already exists on item %2.").arg(o.effect.id, item.id));
  itemRef(next, o.itemId).effects.push_back(o.effect);
  return {EffectRemove{o.itemId, o.effect.id}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const EffectRemove& o) {
  const Item& item = requireItem(doc, o.itemId);
  const auto it = std::find_if(item.effects.begin(), item.effects.end(), [&](const Effect& e) { return e.id == o.effectId; });
  if (it == item.effects.end()) throw OpError(QStringLiteral("Effect %1 not found on item %2.").arg(o.effectId, item.id));
  std::erase_if(itemRef(next, o.itemId).effects, [&](const Effect& e) { return e.id == o.effectId; });
  return {EffectAdd{o.itemId, *it}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const EffectUpdate& o) {
  const Item& item = requireItem(doc, o.itemId);
  const auto it = std::find_if(item.effects.begin(), item.effects.end(), [&](const Effect& e) { return e.id == o.effectId; });
  if (it == item.effects.end()) throw OpError(QStringLiteral("Effect %1 not found on item %2.").arg(o.effectId, item.id));
  EffectUpdate inv{o.itemId, o.effectId, EffectPatch{it->type, it->params}};
  Item& t = itemRef(next, o.itemId);
  Effect& e = *std::find_if(t.effects.begin(), t.effects.end(), [&](const Effect& x) { return x.id == o.effectId; });
  if (o.patch.type) e.type = *o.patch.type;
  if (o.patch.params)
    for (const auto& [k, v] : *o.patch.params) e.params.insert_or_assign(k, v);
  return {inv};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const MaskAdd& o) {
  const Item& item = requireItem(doc, o.itemId);
  if (std::any_of(item.masks.begin(), item.masks.end(), [&](const Mask& m) { return m.id == o.mask.id; }))
    throw OpError(QStringLiteral("Mask %1 already exists on item %2.").arg(o.mask.id, item.id));
  itemRef(next, o.itemId).masks.push_back(o.mask);
  return {MaskRemove{o.itemId, o.mask.id}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const MaskRemove& o) {
  const Item& item = requireItem(doc, o.itemId);
  const auto it = std::find_if(item.masks.begin(), item.masks.end(), [&](const Mask& m) { return m.id == o.maskId; });
  if (it == item.masks.end()) throw OpError(QStringLiteral("Mask %1 not found on item %2.").arg(o.maskId, item.id));
  std::erase_if(itemRef(next, o.itemId).masks, [&](const Mask& m) { return m.id == o.maskId; });
  return {MaskAdd{o.itemId, *it}};
}

// ---------- markers ----------

Inverse run(TimelineDoc& next, const TimelineDoc&, const MarkerAdd& o) {
  next.markers.push_back(o.marker);
  return {MarkerRemove{o.marker.id}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const MarkerRemove& o) {
  const auto it = std::find_if(doc.markers.begin(), doc.markers.end(), [&](const Marker& m) { return m.id == o.markerId; });
  if (it == doc.markers.end()) throw OpError(QStringLiteral("Marker %1 not found.").arg(o.markerId));
  next.markers.erase(next.markers.begin() + (it - doc.markers.begin()));
  return {MarkerAdd{*it}};
}

Inverse run(TimelineDoc& next, const TimelineDoc& doc, const MarkerUpdate& o) {
  const auto it = std::find_if(doc.markers.begin(), doc.markers.end(), [&](const Marker& m) { return m.id == o.markerId; });
  if (it == doc.markers.end()) throw OpError(QStringLiteral("Marker %1 not found.").arg(o.markerId));
  MarkerUpdate inv{o.markerId, MarkerPatch{it->frame, it->label, it->color}};
  Marker& m = next.markers[static_cast<std::size_t>(it - doc.markers.begin())];
  if (o.patch.frame) m.frame = *o.patch.frame;
  if (o.patch.label) m.label = *o.patch.label;
  if (o.patch.color) m.color = *o.patch.color;
  return {inv};
}

} // namespace

ApplyResult applyOps(const TimelineDoc& doc, const std::vector<Op>& ops, ApplyOptions opts) {
  TimelineDoc current = doc;
  Inverse inverses;
  for (const Op& op : ops) {
    ApplyResult r = applyOp(current, op, opts);
    current = std::move(r.doc);
    inverses.insert(inverses.begin(), r.inverse.begin(), r.inverse.end());
  }
  return {std::move(current), std::move(inverses)};
}

ApplyResult applyOp(const TimelineDoc& doc, const Op& op, ApplyOptions opts) {
  validateOp(op); // throws SchemaError on a malformed op
  if (opts.enforceLocks) assertTracksUnlocked(doc, op);
  if (const auto* batch = std::get_if<BatchOp>(&op.body)) {
    for (const Op& inner : batch->ops)
      if (std::holds_alternative<BatchOp>(inner.body))
        throw OpError(QStringLiteral("Nested batch ops are not allowed."), QStringLiteral("Flatten the op list."));
    return applyOps(doc, batch->ops, opts);
  }
  TimelineDoc next = doc;
  Inverse inverse = std::visit(
      [&](const auto& o) -> Inverse {
        if constexpr (std::is_same_v<std::decay_t<decltype(o)>, BatchOp>) return {}; // handled above
        else return run(next, doc, o);
      },
      op.body);
  return {std::move(next), std::move(inverse)};
}

double effectiveSpeed(const Item& item) {
  if (item.timeRemap.size() >= 2) {
    const TimeRemapPoint& first = item.timeRemap.front();
    const TimeRemapPoint& last = item.timeRemap.back();
    const Frame dur = std::max<Frame>(1, last.frame - first.frame);
    return static_cast<double>(last.sourceFrame - first.sourceFrame) / static_cast<double>(dur);
  }
  return item.speed;
}

} // namespace sf
