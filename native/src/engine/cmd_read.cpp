// Read commands: timeline.get, item.get, assets.list, asset.probe, selection.get/set, history.list.

#include "engine/cmd_util.h"
#include "media/probe.h"

#include <QFileInfo>

#include <algorithm>

namespace sf::engine {

namespace {

QJsonObject projectHeader(const TimelineDoc& d) {
  return QJsonObject{{q("name"), d.project.name}, {q("fps"), static_cast<double>(d.project.fps)}, {q("width"), static_cast<double>(d.project.width)},
                     {q("height"), static_cast<double>(d.project.height)}, {q("durationFrames"), static_cast<double>(docDurationFrames(d))}};
}

QJsonObject compactItem(const Item& it) {
  QJsonObject o{{q("id"), it.id}, {q("type"), enumName(it.type())}, {q("start"), static_cast<double>(it.startFrame)}, {q("dur"), static_cast<double>(it.durationFrames)}};
  if (it.assetId) o.insert(q("asset"), *it.assetId);
  if (it.sourceInFrame && *it.sourceInFrame != 0) o.insert(q("srcIn"), static_cast<double>(*it.sourceInFrame));
  if (it.speed != 1) o.insert(q("speed"), it.speed);
  if (it.volume != 1) o.insert(q("vol"), it.volume);
  if (it.muted) o.insert(q("muted"), true);
  if (!it.effects.empty()) o.insert(q("fx"), static_cast<int>(it.effects.size()));
  if (it.labels.name) o.insert(q("name"), *it.labels.name);
  if (const auto* t = std::get_if<TextProps>(&it.props)) o.insert(q("text"), t->text.left(60));
  if (const auto* cp = std::get_if<CaptionProps>(&it.props)) o.insert(q("words"), static_cast<int>(cp->words.size()));
  return o;
}

Result timelineGet(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  const bool detail = a.value(q("mode")).toString(q("summary")) == QLatin1String("detail");
  QStringList trackFilter, typeFilter;
  for (const QJsonValue& v : a.value(q("trackIds")).toArray()) trackFilter << v.toString();
  for (const QJsonValue& v : a.value(q("itemTypes")).toArray()) typeFilter << v.toString();
  const bool hasRange = a.contains(q("startFrame")) || a.contains(q("endFrame"));
  const Frame from = a.value(q("startFrame")).toInteger(0);
  const Frame to = a.value(q("endFrame")).toInteger(std::numeric_limits<Frame>::max());
  const qsizetype limit = a.value(q("limit")).toInt(1000);
  for (const QString& t : trackFilter)
    if (!getTrack(d, t)) return notFound(QStringLiteral("Track %1 not found.").arg(t), QStringLiteral("Call timeline.get without trackIds to list tracks."));

  QJsonObject out{{q("project"), projectHeader(d)}};
  QJsonArray tracks;
  qsizetype emitted = 0;
  bool truncated = false;
  for (const Track& t : d.tracks) {
    if (!trackFilter.isEmpty() && !trackFilter.contains(t.id)) continue;
    QJsonObject tj = trackJson(t);
    QJsonArray items;
    int total = 0;
    for (const Item* it : itemsOnTrack(d, t.id)) {
      ++total;
      if (hasRange && (itemEnd(*it) <= from || it->startFrame >= to)) continue;
      if (!typeFilter.isEmpty() && !typeFilter.contains(enumName(it->type()))) continue;
      if (emitted >= limit) {
        truncated = true;
        continue;
      }
      items.append(detail ? QJsonValue(toJson(*it)) : QJsonValue(compactItem(*it)));
      ++emitted;
    }
    tj.insert(q("itemCount"), total);
    tj.insert(q("items"), items);
    tracks.append(tj);
  }
  out.insert(q("tracks"), tracks);
  if (a.value(q("includeMarkers")).toBool(true)) {
    QJsonArray markers;
    for (const Marker& m : d.markers)
      if (!hasRange || (m.frame >= from && m.frame < to)) markers.append(toJson(m));
    out.insert(q("markers"), markers);
  }
  if (truncated) out.insert(q("truncated"), true);
  if (detail && d.mixer) out.insert(q("mixer"), toJson(*d.mixer));
  return Result::success(out);
}

} // namespace

void registerReadCommands(Engine& e) {
  e.add(spec("timeline.get", "getTimeline", "engine", false,
             QStringLiteral("Read the timeline. mode \"summary\" (default): tracks with compact items (id, type, start, dur, asset, name...) - cheap in tokens; \"detail\": full item JSON "
                            "(transforms, effects, props). Filter with trackIds, itemTypes and a [startFrame, endFrame) range. ALWAYS read back after editing."),
             js::obj({{q("mode"), js::choice({q("summary"), q("detail")})},
                      {q("trackIds"), js::array(js::str())},
                      {q("itemTypes"), js::array(js::choice({q("video"), q("audio"), q("image"), q("text"), q("caption"), q("shape"), q("motionGraphic")}))},
                      {q("startFrame"), js::integer(QStringLiteral("Only items overlapping from this timeline frame"), 0)},
                      {q("endFrame"), js::integer(QStringLiteral("... up to (excluding) this frame"), 0)},
                      {q("includeMarkers"), js::boolean()},
                      {q("limit"), js::integer(QStringLiteral("Max items returned (default 1000)"), 1)}}),
             timelineGet));

  e.add(spec("item.get", "", "engine", false, QStringLiteral("One item in full (all props, effects, masks, keyframes)."), js::obj({{q("itemId"), js::str()}}, {q("itemId")}),
             [](CallContext& c, const QJsonObject& a) {
               const Item* it = getItem(c.session->project->doc(), a.value(q("itemId")).toString());
               if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(a.value(q("itemId")).toString()), QStringLiteral("Call timeline.get."));
               QJsonObject o = toJson(*it);
               o.insert(q("endFrame"), static_cast<double>(itemEnd(*it)));
               return Result::success(o);
             }));

  e.add(spec("assets.list", "listAssets", "engine", false, QStringLiteral("List the project's media assets (id, kind, name, path, status, duration, size, proxy state, uses)."),
             js::obj({{q("kind"), js::choice({q("video"), q("audio"), q("image")})}}),
             [](CallContext& c, const QJsonObject& a) {
               const QString kind = a.value(q("kind")).toString();
               QJsonArray arr;
               for (const Asset& as : c.session->project->assets())
                 if (kind.isEmpty() || enumName(as.kind) == kind) arr.append(assetBrief(as, c.session->pool));
               return Result::success(QJsonObject{{q("assets"), arr}});
             }));

  e.add(spec("asset.probe", "getAsset", "engine", false,
             QStringLiteral("Probe a media file (or an asset's file) with FFmpeg: duration, size, fps, codecs, rotation, colour tags, audio format. Does not add it to the project."),
             js::obj({{q("path"), js::str()}, {q("assetId"), js::str()}}),
             [](CallContext& c, const QJsonObject& a) {
               QString path = a.value(q("path")).toString();
               if (path.isEmpty()) {
                 Session* s = c.engine->session(a.value(q("session")).toString());
                 const Asset* as = s ? s->project->asset(a.value(q("assetId")).toString()) : nullptr;
                 if (!as) return invalid(QStringLiteral("Pass path, or the assetId of an asset in an open project."));
                 path = as->path;
               }
               QString err;
               const auto info = probeMedia(path, &err);
               if (!info) return Result::failure(QFileInfo::exists(path) ? q("unsupported") : q("not_found"), err.isEmpty() ? QStringLiteral("Can't probe %1").arg(path) : err);
               return Result::success(QJsonObject{{q("path"), path}, {q("durationMs"), static_cast<double>(info->durationMs)}, {q("hasVideo"), info->hasVideo}, {q("hasAudio"), info->hasAudio},
                                                  {q("width"), info->displayWidth}, {q("height"), info->displayHeight}, {q("fps"), info->fps}, {q("avgFps"), info->avgFps},
                                                  {q("vfr"), info->vfr}, {q("videoCodec"), info->videoCodec}, {q("audioCodec"), info->audioCodec},
                                                  {q("audioSampleRate"), info->audioSampleRate}, {q("audioChannels"), info->audioChannels}, {q("rotation"), info->rotation},
                                                  {q("pixelFormat"), info->pixelFormat}, {q("bitDepth"), info->bitDepth},
                                                  {q("colorTransfer"), info->color.transfer}, {q("colorPrimaries"), info->color.primaries}, {q("colorMatrix"), info->color.matrix}});
             },
             false));

  e.add(spec("selection.get", "", "engine", false, QStringLiteral("The selected item ids."), js::obj({}),
             [](CallContext& c, const QJsonObject&) { return Result::success(QJsonObject{{q("selection"), QJsonArray::fromStringList(c.session->project->selection())}}); }));
  e.add(spec("selection.set", "", "engine", false, QStringLiteral("Replace the selection (ids that don't exist are dropped)."), js::obj({{q("ids"), js::array(js::str())}}, {q("ids")}),
             [](CallContext& c, const QJsonObject& a) {
               QStringList ids;
               for (const QJsonValue& v : a.value(q("ids")).toArray())
                 if (getItem(c.session->project->doc(), v.toString())) ids << v.toString();
               c.session->project->setSelection(ids);
               return Result::success(QJsonObject{{q("selection"), QJsonArray::fromStringList(c.session->project->selection())}});
             }));

  e.add(spec("history.list", "", "engine", false,
             QStringLiteral("Undo history, oldest first: index, label, actor (origin), number of ops, whether it is currently applied. `position` = number of applied groups."),
             js::obj({{q("limit"), js::integer(QStringLiteral("Newest N entries (default 50)"), 1, 1000)}}),
             [](CallContext& c, const QJsonObject& a) {
               const History& h = c.session->project->history();
               const auto& groups = h.groups();
               const qsizetype limit = a.value(q("limit")).toInt(50);
               const qsizetype first = std::max<qsizetype>(0, static_cast<qsizetype>(groups.size()) - limit);
               QJsonArray arr;
               for (qsizetype i = first; i < static_cast<qsizetype>(groups.size()); ++i) {
                 const UndoGroup& g = groups[static_cast<size_t>(i)];
                 QJsonArray types;
                 for (const Op& o : g.ops) types.append(o.type());
                 QJsonObject o{{q("index"), static_cast<int>(i)}, {q("label"), g.label.value_or(QString())}, {q("actor"), g.actor.value_or(QString())},
                               {q("ops"), static_cast<int>(g.ops.size())}, {q("opTypes"), types}, {q("applied"), i < static_cast<qsizetype>(h.position())}};
                 if (g.createdAt) o.insert(q("createdAt"), static_cast<double>(*g.createdAt));
                 arr.append(o);
               }
               return Result::success(QJsonObject{{q("entries"), arr}, {q("position"), static_cast<int>(h.position())}, {q("total"), static_cast<int>(groups.size())},
                                                  {q("canUndo"), h.canUndo()}, {q("canRedo"), h.canRedo()}});
             }));
}

} // namespace sf::engine
