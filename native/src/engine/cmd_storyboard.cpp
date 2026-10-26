#include "engine/cmd_storyboard.h"
#include "engine/cmd_util.h"
#include "core/apply.h"
#include "core/ops.h"
#include "core/schema.h"
#include "core/timeline_doc.h"

#include <cmath>

namespace sf::engine {

namespace {

StoryboardScene parseSceneFromArgs(const QJsonObject& a) {
  QJsonObject sObj = a;
  if (a.contains(q("scene")) && a.value(q("scene")).isObject()) {
    sObj = a.value(q("scene")).toObject();
  }
  StoryboardScene s;
  s.id = sObj.value(q("id")).toString();
  if (s.id.isEmpty()) s.id = newId(QStringLiteral("scn"));
  s.title = sObj.value(q("title")).toString(QStringLiteral("Untitled Scene"));
  s.intent = sObj.value(q("intent")).toString();
  if (sObj.contains(q("durationSec"))) {
    s.durationSec = std::max(0.05, sObj.value(q("durationSec")).toDouble(5.0));
  } else {
    s.durationSec = 5.0;
  }
  if (sObj.contains(q("shotPrompt"))) s.shotPrompt = sObj.value(q("shotPrompt")).toString();
  if (sObj.contains(q("voText"))) s.voText = sObj.value(q("voText")).toString();
  if (sObj.contains(q("musicCue"))) s.musicCue = sObj.value(q("musicCue")).toString();
  if (sObj.contains(q("colourNote"))) s.colourNote = sObj.value(q("colourNote")).toString();
  if (sObj.contains(q("status"))) {
    s.status = sObj.value(q("status")).toString();
  } else {
    s.status = QStringLiteral("planned");
  }
  if (sObj.contains(q("assetIds")) && sObj.value(q("assetIds")).isArray()) {
    for (const auto& v : sObj.value(q("assetIds")).toArray()) {
      const QString aid = v.toString();
      if (!aid.isEmpty()) s.assetIds.push_back(aid);
    }
  }
  return s;
}

// The fields of one scene, shared by storyboard.set, scene.add and scene.update so the validator
// accepts what parseSceneFromArgs reads.
QJsonObject sceneSchema() {
  return js::obj({{q("id"), js::str()},
                  {q("title"), js::str()},
                  {q("intent"), js::str()},
                  {q("durationSec"), js::number()},
                  {q("shotPrompt"), js::str()},
                  {q("voText"), js::str()},
                  {q("musicCue"), js::str()},
                  {q("colourNote"), js::str()},
                  {q("status"), js::str()},
                  {q("assetIds"), js::array(js::str())}});
}

QJsonArray scenesToJsonArray(const std::vector<StoryboardScene>& scenes) {
  QJsonArray arr;
  for (const auto& s : scenes) {
    arr.append(toJson(s));
  }
  return arr;
}

} // namespace

void registerStoryboardCommands(Engine& e) {
  // storyboard.get
  e.add(spec("storyboard.get", "getStoryboard", "editor", false,
             QStringLiteral("Get the current project storyboard (list of scenes)."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               const auto& scenes = c.session->project->doc().storyboard;
               return Result::success(QJsonObject{
                 {q("scenes"), scenesToJsonArray(scenes)},
                 {q("storyboard"), scenesToJsonArray(scenes)},
                 {q("count"), static_cast<int>(scenes.size())}
               });
             }));

  // storyboard.set
  e.add(spec("storyboard.set", "setStoryboard", "editor", true,
             QStringLiteral("Replace the entire project storyboard (undoable)."),
             js::obj({{q("scenes"), js::array(sceneSchema())}, {q("storyboard"), js::array(sceneSchema())}}, {q("scenes")}),
             [](CallContext& c, const QJsonObject& a) {
               QJsonArray arr = a.value(q("scenes")).toArray();
               if (arr.isEmpty() && a.contains(q("storyboard")) && a.value(q("storyboard")).isArray()) {
                 arr = a.value(q("storyboard")).toArray();
               }
               std::vector<StoryboardScene> scenes;
               for (const auto& v : arr) {
                 if (v.isObject()) {
                   scenes.push_back(parseSceneFromArgs(v.toObject()));
                 }
               }
               std::vector<Op> ops{StoryboardSet{scenes}};
               Result res = applyToSession(c, ops, QStringLiteral("storyboard.set"));
               if (!res.ok) return res;
               return Result::success(QJsonObject{
                 {q("scenes"), scenesToJsonArray(scenes)},
                 {q("storyboard"), scenesToJsonArray(scenes)},
                 {q("count"), static_cast<int>(scenes.size())}
               });
             }));

  // storyboard.scene.add
  e.add(spec("storyboard.scene.add", "addStoryboardScene", "editor", true,
             QStringLiteral("Add a scene to the storyboard (undoable)."),
             js::obj({{q("id"), js::str()},
                      {q("title"), js::str()},
                      {q("intent"), js::str()},
                      {q("durationSec"), js::number()},
                      {q("shotPrompt"), js::str()},
                      {q("voText"), js::str()},
                      {q("musicCue"), js::str()},
                      {q("colourNote"), js::str()},
                      {q("status"), js::str()},
                      {q("assetIds"), js::array(js::str())},
                      {q("scene"), sceneSchema()},
                      {q("index"), js::integer(QStringLiteral("0-based insertion index"))}}),
             [](CallContext& c, const QJsonObject& a) {
               StoryboardScene newScene = parseSceneFromArgs(a);
               std::vector<StoryboardScene> scenes = c.session->project->doc().storyboard;
               int idx = static_cast<int>(scenes.size());
               if (a.contains(q("index"))) {
                 int requested = a.value(q("index")).toInt();
                 if (requested >= 0 && requested <= static_cast<int>(scenes.size())) {
                   idx = requested;
                 }
               }
               scenes.insert(scenes.begin() + idx, newScene);
               std::vector<Op> ops{StoryboardSet{scenes}};
               Result res = applyToSession(c, ops, QStringLiteral("storyboard.scene.add"));
               if (!res.ok) return res;
               return Result::success(QJsonObject{
                 {q("scene"), toJson(newScene)},
                 {q("scenes"), scenesToJsonArray(scenes)},
                 {q("count"), static_cast<int>(scenes.size())}
               });
             }));

  // storyboard.scene.update
  e.add(spec("storyboard.scene.update", "updateStoryboardScene", "editor", true,
             QStringLiteral("Update a scene in the storyboard (undoable)."),
             js::obj({{q("id"), js::str()},
                      {q("title"), js::str()},
                      {q("intent"), js::str()},
                      {q("durationSec"), js::number()},
                      {q("shotPrompt"), js::str()},
                      {q("voText"), js::str()},
                      {q("musicCue"), js::str()},
                      {q("colourNote"), js::str()},
                      {q("status"), js::str()},
                      {q("assetIds"), js::array(js::str())},
                      {q("patch"), sceneSchema()}}, {q("id")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("id")).toString();
               if (id.isEmpty()) return invalid(QStringLiteral("Argument 'id' is required"));
               QJsonObject patch = a.value(q("patch")).toObject();
               if (patch.isEmpty()) patch = a;

               std::vector<StoryboardScene> scenes = c.session->project->doc().storyboard;
               auto it = std::find_if(scenes.begin(), scenes.end(), [&](const StoryboardScene& s) {
                 return s.id == id;
               });
               if (it == scenes.end()) {
                 return notFound(QStringLiteral("Scene '%1' not found").arg(id));
               }

               if (patch.contains(q("title"))) it->title = patch.value(q("title")).toString();
               if (patch.contains(q("intent"))) it->intent = patch.value(q("intent")).toString();
               if (patch.contains(q("durationSec"))) it->durationSec = std::max(0.05, patch.value(q("durationSec")).toDouble(it->durationSec));
               if (patch.contains(q("shotPrompt"))) it->shotPrompt = patch.value(q("shotPrompt")).toString();
               if (patch.contains(q("voText"))) it->voText = patch.value(q("voText")).toString();
               if (patch.contains(q("musicCue"))) it->musicCue = patch.value(q("musicCue")).toString();
               if (patch.contains(q("colourNote"))) it->colourNote = patch.value(q("colourNote")).toString();
               if (patch.contains(q("status"))) it->status = patch.value(q("status")).toString();
               if (patch.contains(q("assetIds")) && patch.value(q("assetIds")).isArray()) {
                 it->assetIds.clear();
                 for (const auto& v : patch.value(q("assetIds")).toArray()) {
                   const QString aid = v.toString();
                   if (!aid.isEmpty()) it->assetIds.push_back(aid);
                 }
               }

               StoryboardScene updated = *it;
               std::vector<Op> ops{StoryboardSet{scenes}};
               Result res = applyToSession(c, ops, QStringLiteral("storyboard.scene.update"));
               if (!res.ok) return res;
               return Result::success(QJsonObject{
                 {q("scene"), toJson(updated)},
                 {q("scenes"), scenesToJsonArray(scenes)},
                 {q("count"), static_cast<int>(scenes.size())}
               });
             }));

  // storyboard.scene.remove
  e.add(spec("storyboard.scene.remove", "removeStoryboardScene", "editor", true,
             QStringLiteral("Remove a scene from the storyboard (undoable)."),
             js::obj({{q("id"), js::str()}}, {q("id")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("id")).toString();
               if (id.isEmpty()) return invalid(QStringLiteral("Argument 'id' is required"));

               std::vector<StoryboardScene> scenes = c.session->project->doc().storyboard;
               auto it = std::find_if(scenes.begin(), scenes.end(), [&](const StoryboardScene& s) {
                 return s.id == id;
               });
               if (it == scenes.end()) {
                 return notFound(QStringLiteral("Scene '%1' not found").arg(id));
               }

               scenes.erase(it);
               std::vector<Op> ops{StoryboardSet{scenes}};
               Result res = applyToSession(c, ops, QStringLiteral("storyboard.scene.remove"));
               if (!res.ok) return res;
               return Result::success(QJsonObject{
                 {q("removed"), id},
                 {q("scenes"), scenesToJsonArray(scenes)},
                 {q("count"), static_cast<int>(scenes.size())}
               });
             }));

  // storyboard.toTimeline
  e.add(spec("storyboard.toTimeline", "storyboardToTimeline", "editor", true,
             QStringLiteral("Lays scenes out as placeholder slots on the timeline, with markers (undoable)."),
             js::obj({{q("trackName"), js::str(QStringLiteral("Optional name of a text track for the placeholders"))},
                      {q("startFrame"), js::integer(QStringLiteral("Starting frame position (default 0)"))}}),
             [](CallContext& c, const QJsonObject& a) {
               const TimelineDoc& d = c.session->project->doc();
               if (d.storyboard.empty()) {
                 return invalid(QStringLiteral("Storyboard has no scenes to lay out. Add scenes first with storyboard.scene.add."));
               }

               const double fps = (d.project.fps > 0) ? static_cast<double>(d.project.fps) : 30.0;
               Frame curFrame = static_cast<Frame>(a.value(q("startFrame")).toInt(0));

               // Find an existing text track or create one
               QString textTrackId;
               const QString customName = a.value(q("trackName")).toString();
               // A name that already belongs to a non-text track must not be reused for a text
               // track (two tracks would share one name): ignore it and fall back to the default.
               bool ignoreCustomName = false;
               if (!customName.isEmpty()) {
                 for (const Track& t : d.tracks)
                   if (t.name == customName && t.kind != TrackKind::Text) {
                     ignoreCustomName = true;
                     break;
                   }
               }
               const QString effectiveName = ignoreCustomName ? QString() : customName;

               for (const Track& t : d.tracks) {
                 if (!t.locked && t.kind == TrackKind::Text) {
                   if (effectiveName.isEmpty() || t.name == effectiveName) {
                     textTrackId = t.id;
                     break;
                   }
                 }
               }

               std::vector<Op> ops;
               if (textTrackId.isEmpty()) {
                 textTrackId = newId(QStringLiteral("trk"));
                 ops.push_back(TrackAdd{
                   .trackId = textTrackId,
                   .kind = TrackKind::Text,
                   .name = !effectiveName.isEmpty() ? effectiveName : QStringLiteral("Storyboard Slots")
                 });
               }

               std::vector<StoryboardScene> updatedScenes = d.storyboard;
               int markersAdded = 0;
               int placedCount = 0;

               for (auto& scene : updatedScenes) {
                 const Frame durFrames = std::max(Frame(1), static_cast<Frame>(std::lround(scene.durationSec * fps)));
                 const QString itemId = newId(QStringLiteral("itm"));

                 TextProps tp;
                 tp.text = QStringLiteral("[%1] %2\n%3").arg(scene.id, scene.title, scene.intent);
                 tp.style.fontFamily = QStringLiteral("Geist");
                 tp.style.fontSize = 48;
                 tp.style.fontWeight = 700;
                 tp.style.color = QStringLiteral("#ffffff");
                 Item item = createItem(ItemType::Text, ItemInit{
                   .id = itemId,
                   .trackId = textTrackId,
                   .startFrame = curFrame,
                   .durationFrames = durFrames,
                   .props = tp
                 });
                 ops.push_back(ItemAdd{item});

                 Marker marker;
                 marker.id = newId(QStringLiteral("mrk"));
                 marker.frame = curFrame;
                 marker.label = scene.title;
                 marker.color = QStringLiteral("#00bcd4");
                 ops.push_back(MarkerAdd{marker});
                 ++markersAdded;

                 scene.status = QStringLiteral("placed");
                 curFrame += durFrames;
                 ++placedCount;
               }

               ops.push_back(StoryboardSet{updatedScenes});

               Result res = applyToSession(c, ops, QStringLiteral("storyboard.toTimeline"));
               if (!res.ok) return res;

               return Result::success(QJsonObject{
                 {q("placedScenes"), placedCount},
                 {q("totalFrames"), static_cast<qint64>(curFrame)},
                 {q("markersAdded"), markersAdded},
                 {q("trackId"), textTrackId}
               });
             }));
}

} // namespace sf::engine
