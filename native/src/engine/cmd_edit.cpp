// Write commands: ops.apply (raw core ops), the facade tools ported from the Electron agent (addClip, addText, splitItem, ...),
// colour (item.color.set), audio (mixer.*, item.audio.set), undo / redo.

#include "engine/cmd_util.h"

#include <algorithm>

namespace sf::engine {

namespace {

const QJsonObject kFrame = js::integer(QStringLiteral("Timeline frame"), 0);

QJsonArray itemsJson(const TimelineDoc& d, const QStringList& ids) {
  QJsonArray arr;
  for (const QString& id : ids)
    if (const Item* it = getItem(d, id)) arr.append(itemSummary(d, *it));
  return arr;
}

// Applies one batch and answers with the affected items as they are afterwards.
Result applyAndShow(CallContext& c, const std::vector<Op>& ops, const QString& label, const QStringList& itemIds, QJsonObject extra = {}) {
  Result r = applyToSession(c, ops, label, extra);
  if (!r.ok) return r;
  QJsonObject o = r.value.toObject();
  o.insert(q("items"), itemsJson(c.session->project->doc(), itemIds));
  return Result::success(o);
}

const Track* firstTrackFor(const TimelineDoc& d, ItemType type, const QString& wanted, Result* err) {
  if (!wanted.isEmpty()) {
    const Track* t = getTrack(d, wanted);
    if (!t) {
      *err = notFound(QStringLiteral("Track %1 not found.").arg(wanted), QStringLiteral("Call timeline.get to list tracks."));
      return nullptr;
    }
    if (t->locked) {
      *err = Result::failure(q("edit_rejected"), QStringLiteral("Track \"%1\" is locked. Unlock it first (track.update via ops.apply).").arg(t->name));
      return nullptr;
    }
    if (!trackAllowsItem(t->kind, type)) {
      *err = invalid(QStringLiteral("Track \"%1\" (%2) cannot hold %3 items.").arg(t->name, enumName(t->kind), enumName(type)));
      return nullptr;
    }
    return t;
  }
  for (const Track& t : d.tracks)
    if (!t.locked && trackAllowsItem(t.kind, type)) return &t;
  *err = Result::failure(q("edit_rejected"), QStringLiteral("No unlocked track can hold a %1 item. Add one with track.add.").arg(enumName(type)));
  return nullptr;
}

Frame trackEnd(const TimelineDoc& d, const QString& trackId) {
  Frame end = 0;
  for (const Item& i : d.items)
    if (i.trackId == trackId) end = std::max(end, itemEnd(i));
  return end;
}

Result addMedia(CallContext& c, const QJsonObject& a, bool audioOnly) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  const Asset* asset = p.asset(a.value(q("assetId")).toString());
  if (!asset) return notFound(QStringLiteral("Asset %1 not found.").arg(a.value(q("assetId")).toString()), QStringLiteral("Call assets.list first."));
  if (audioOnly && asset->kind != AssetKind::Audio)
    return invalid(QStringLiteral("Asset %1 is %2, not audio. Use clip.add for video and images.").arg(asset->id, enumName(asset->kind)));
  const ItemType type = asset->kind == AssetKind::Audio ? ItemType::Audio : asset->kind == AssetKind::Image ? ItemType::Image : ItemType::Video;
  Result err;
  const Track* track = firstTrackFor(d, type, a.value(q("trackId")).toString(), &err);
  if (!track) return err;
  const double fps = static_cast<double>(d.project.fps);
  const Frame start = std::max<Frame>(0, a.contains(q("startFrame")) ? a.value(q("startFrame")).toInteger() : (audioOnly ? 0 : trackEnd(d, track->id)));
  const Frame srcIn = a.value(q("sourceInFrame")).toInteger(0);
  const Frame assetFrames = asset->durationMs > 0 ? jsRound(static_cast<double>(asset->durationMs) / 1000.0 * fps) : jsRound(fps);
  const Frame dur = a.contains(q("durationFrames")) ? a.value(q("durationFrames")).toInteger()
                                                    : (type == ItemType::Image ? jsRound(fps * 5) : std::max<Frame>(1, assetFrames - srcIn));
  ItemInit init;
  init.id = newId(q("itm"));
  init.trackId = track->id;
  init.startFrame = start;
  init.durationFrames = dur;
  init.assetId = asset->id;
  if (type != ItemType::Image) init.sourceInFrame = srcIn;
  if (a.contains(q("volume"))) init.volume = a.value(q("volume")).toDouble();
  init.labels = Labels{audioOnly ? QStringLiteral("audio") : QStringLiteral("clip"), std::nullopt};
  const Item item = createItem(type, init);
  return applyAndShow(c, {Op(ItemAdd{item, false})}, audioOnly ? QStringLiteral("addAudio") : QStringLiteral("addClip"), {item.id});
}

TextStyle textStyleFrom(const QJsonObject& s) {
  TextStyle t;
  t.fontFamily = s.value(q("fontFamily")).toString(QStringLiteral("Geist"));
  t.fontSize = s.value(q("fontSize")).toDouble(72);
  t.fontWeight = s.value(q("fontWeight")).toInteger(800);
  t.color = s.value(q("color")).toString(QStringLiteral("#ffffff"));
  if (s.contains(q("strokeColor"))) t.strokeColor = s.value(q("strokeColor")).toString();
  t.strokeWidth = s.value(q("strokeWidth")).toDouble(4);
  if (s.contains(q("backgroundColor"))) t.backgroundColor = s.value(q("backgroundColor")).toString();
  const QString al = s.value(q("align")).toString(QStringLiteral("center"));
  t.align = al == QLatin1String("left") ? TextAlign::Left : al == QLatin1String("right") ? TextAlign::Right : TextAlign::Center;
  t.uppercase = s.value(q("uppercase")).toBool(false);
  return t;
}

const QJsonObject kTextStyleSchema = js::obj({{q("fontFamily"), js::str()}, {q("fontSize"), js::number(QString(), 1, 2000)}, {q("fontWeight"), js::integer(QString(), 100, 1000)},
                                              {q("color"), js::str(QStringLiteral("CSS colour, e.g. #ffffff"))}, {q("strokeColor"), js::str()}, {q("strokeWidth"), js::number(QString(), 0)},
                                              {q("backgroundColor"), js::str()}, {q("align"), js::choice({q("left"), q("center"), q("right")})}, {q("uppercase"), js::boolean()}});

Result addText(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  const QString text = a.value(q("text")).toString();
  Result err;
  const Track* track = firstTrackFor(d, ItemType::Text, a.value(q("trackId")).toString(), &err);
  if (!track) return err;
  ItemInit init;
  init.id = newId(q("itm"));
  init.trackId = track->id;
  init.startFrame = std::max<Frame>(0, a.value(q("startFrame")).toInteger());
  init.durationFrames = a.value(q("durationFrames")).toInteger(1);
  TransformPatch tp;
  tp.x = a.value(q("x")).toDouble(0);
  tp.y = a.value(q("y")).toDouble(0);
  init.transform = tp;
  init.labels = Labels{text.left(24), std::nullopt};
  init.props = ItemProps{TextProps{text, textStyleFrom(a.value(q("style")).toObject())}};
  const Item item = createItem(ItemType::Text, init);
  return applyAndShow(c, {Op(ItemAdd{item, false})}, QStringLiteral("addText"), {item.id});
}

Result addShape(CallContext& c, const QJsonObject& a) {
  const TimelineDoc& d = c.session->project->doc();
  Result err;
  const Track* track = firstTrackFor(d, ItemType::Shape, a.value(q("trackId")).toString(), &err);
  if (!track) return err;
  ShapeProps sp;
  const QString kind = a.value(q("shape")).toString(QStringLiteral("rect"));
  sp.shape = kind == QLatin1String("ellipse") ? ShapeKind::Ellipse : kind == QLatin1String("triangle") ? ShapeKind::Triangle : ShapeKind::Rect;
  sp.fill = a.value(q("fill")).toString(QStringLiteral("#ffffff"));
  if (a.contains(q("stroke"))) sp.stroke = a.value(q("stroke")).toString();
  sp.strokeWidth = a.value(q("strokeWidth")).toDouble(0);
  sp.radius = a.value(q("radius")).toDouble(0);
  sp.width = a.value(q("width")).toDouble(static_cast<double>(d.project.width) / 4);
  sp.height = a.value(q("height")).toDouble(static_cast<double>(d.project.height) / 4);
  ItemInit init;
  init.id = newId(q("itm"));
  init.trackId = track->id;
  init.startFrame = std::max<Frame>(0, a.value(q("startFrame")).toInteger());
  init.durationFrames = a.value(q("durationFrames")).toInteger(1);
  TransformPatch tp;
  tp.x = a.value(q("x")).toDouble(0);
  tp.y = a.value(q("y")).toDouble(0);
  if (a.contains(q("opacity"))) tp.opacity = a.value(q("opacity")).toDouble();
  init.transform = tp;
  init.labels = Labels{kind, std::nullopt};
  init.props = ItemProps{sp};
  const Item item = createItem(ItemType::Shape, init);
  return applyAndShow(c, {Op(ItemAdd{item, false})}, QStringLiteral("addShape"), {item.id});
}

struct CapWord {
  QString w;
  double startSec = 0, endSec = 0; // timeline seconds
};

Result addCaptions(CallContext& c, const QJsonObject& a) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& d = p.doc();
  const double fps = static_cast<double>(d.project.fps);
  std::vector<CapWord> words;
  if (a.contains(q("words"))) {
    for (const QJsonValue& v : a.value(q("words")).toArray()) {
      const QJsonObject w = v.toObject();
      words.push_back({w.value(q("w")).toString(), static_cast<double>(w.value(q("startMs")).toInteger()) / 1000.0, static_cast<double>(w.value(q("endMs")).toInteger()) / 1000.0});
    }
  } else if (a.contains(q("assetId"))) {
    const QString assetId = a.value(q("assetId")).toString();
    const Transcript* tr = nullptr;
    for (const Transcript& t : p.bundle().transcripts)
      if (t.assetId == assetId) tr = &t;
    if (!tr) return notFound(QStringLiteral("No transcript for asset %1.").arg(assetId), QStringLiteral("Pass `words` instead (this build has no speech recognition)."));
    for (const Item& it : d.items) {
      if (it.assetId != assetId || !isMediaType(it.type())) continue;
      const double srcStart = static_cast<double>(it.sourceInFrame.value_or(0)) / fps;
      const double srcEnd = static_cast<double>(sourceOutFrame(it)) / fps;
      const double t0 = static_cast<double>(it.startFrame) / fps;
      const double speed = it.speed > 0 ? it.speed : 1;
      for (const TranscriptWord& w : tr->words) {
        const double s = static_cast<double>(w.startMs) / 1000.0, e = static_cast<double>(w.endMs) / 1000.0;
        if (s < srcStart || s >= srcEnd) continue;
        words.push_back({w.w, t0 + (s - srcStart) * speed, t0 + (std::min(e, srcEnd) - srcStart) * speed});
      }
    }
  } else {
    return invalid(QStringLiteral("Pass `words` ([{w, startMs, endMs}] on the timeline) or the `assetId` of an asset that has a transcript."));
  }
  if (words.empty()) return invalid(QStringLiteral("No words to caption."));
  std::sort(words.begin(), words.end(), [](const CapWord& x, const CapWord& y) { return x.startSec < y.startSec; });

  Result err;
  const Track* track = firstTrackFor(d, ItemType::Caption, a.value(q("trackId")).toString(), &err);
  if (!track) return err;

  const QString preset = a.value(q("preset")).toString(QStringLiteral("karaoke"));
  CaptionStyle st;
  st.fontFamily = preset == QLatin1String("serif") ? QStringLiteral("Georgia") : QStringLiteral("Inter");
  st.fontSize = preset == QLatin1String("serif") ? 64 : preset == QLatin1String("bold") ? 72 : 76;
  st.fontWeight = preset == QLatin1String("serif") ? 700 : 900;
  st.color = QStringLiteral("#ffffff");
  st.strokeWidth = preset == QLatin1String("serif") ? 6 : preset == QLatin1String("bold") ? 0 : 8;
  if (preset != QLatin1String("bold")) st.strokeColor = QStringLiteral("#000000");
  st.highlight = preset == QLatin1String("karaoke") ? CaptionHighlight::ActiveWord : CaptionHighlight::None;
  st.uppercase = preset != QLatin1String("serif");
  if (a.contains(q("fontSize"))) st.fontSize = a.value(q("fontSize")).toDouble();
  if (a.contains(q("color"))) st.color = a.value(q("color")).toString();
  st.placementY = a.value(q("placementY")).toDouble(0.82);
  st.align = TextAlign::Center;
  st.lineHeight = 1.2;
  st.padding = 8;
  st.maxCharsPerLine = 32;

  const int per = a.value(q("wordsPerCard")).toInt(5);
  const Frame pad = jsRound(0.1 * fps);
  std::vector<Op> ops;
  QStringList ids;
  for (size_t i = 0; i < words.size(); i += static_cast<size_t>(per)) {
    const size_t n = std::min<size_t>(static_cast<size_t>(per), words.size() - i);
    const Frame start = std::max<Frame>(0, jsRound(words[i].startSec * fps));
    const Frame lastEnd = jsRound(words[i + n - 1].endSec * fps);
    const Frame nextStart = i + n < words.size() ? jsRound(words[i + n].startSec * fps) - 1 : lastEnd + pad;
    const Frame end = std::max(lastEnd, nextStart);
    if (end - start < 1) continue;
    CaptionProps cp;
    cp.style = st;
    cp.mode = CaptionMode::Phrase;
    cp.maxWordsPerCard = per;
    QStringList label;
    for (size_t k = i; k < i + n; ++k) {
      const auto ms = [&](double sec) { return std::max<Ms>(0, jsRound((sec - static_cast<double>(start) / fps) * 1000.0)); };
      cp.words.push_back({words[k].w, ms(words[k].startSec), ms(words[k].endSec), std::nullopt, std::nullopt});
      label << words[k].w;
    }
    ItemInit init;
    init.id = newId(q("itm"));
    init.trackId = track->id;
    init.startFrame = start;
    init.durationFrames = end - start;
    init.labels = Labels{label.join(QLatin1Char(' ')).left(24), std::nullopt};
    init.props = ItemProps{cp};
    ops.push_back(Op(ItemAdd{createItem(ItemType::Caption, init), false}));
    ids << init.id;
  }
  if (ops.empty()) return invalid(QStringLiteral("No caption cards generated."));
  return applyToSession(c, ops, QStringLiteral("addCaptions (%1 cards)").arg(ops.size()), {{q("captionCards"), static_cast<int>(ops.size())}, {q("preset"), preset}});
}

Result historyStep(CallContext& c, const QJsonObject& a, bool undo) {
  editor::Project& p = *c.session->project;
  const int steps = a.value(q("steps")).toInt(1);
  QJsonArray done;
  for (int i = 0; i < steps; ++i) {
    const auto& groups = p.history().groups();
    const size_t pos = p.history().position();
    QJsonObject entry;
    if (undo && pos > 0) entry = QJsonObject{{q("label"), groups[pos - 1].label.value_or(QString())}, {q("actor"), groups[pos - 1].actor.value_or(QString())}};
    else if (!undo && pos < groups.size()) entry = QJsonObject{{q("label"), groups[pos].label.value_or(QString())}, {q("actor"), groups[pos].actor.value_or(QString())}};
    if (!(undo ? p.undo() : p.redo())) break;
    done.append(entry);
  }
  return Result::success(QJsonObject{{q("steps"), done.size()}, {undo ? q("undone") : q("redone"), done}, {q("canUndo"), p.canUndo()}, {q("canRedo"), p.canRedo()},
                                     {q("note"), done.isEmpty() ? QStringLiteral("Nothing to %1.").arg(undo ? q("undo") : q("redo")) : QStringLiteral("Verify with timeline.get.")}});
}

MixNode* findNode(Mixer& m, const QString& id) {
  if (id == QLatin1String("master")) return &m.master;
  for (MixNode& n : m.strips)
    if (n.id == id) return &n;
  for (MixNode& n : m.buses)
    if (n.id == id) return &n;
  return nullptr;
}

} // namespace

void registerEditCommands(Engine& e) {
  e.add(spec("ops.apply", "batchEdit", "editor", true,
             QStringLiteral("Apply raw core ops (the same JSON the project history stores: item.add, item.move, track.add, marker.add, project.setCanvas, mixer.set, ... or batch) as ONE atomic "
                            "undo step: if any op is invalid nothing changes. Returns the inverse ops and the ids created. Prefer the specific commands (clip.add, item.split, ...) when one fits."),
             js::obj({{q("ops"), [] { QJsonObject o = js::array(js::obj({{q("type"), js::str(QStringLiteral("Op discriminator, e.g. \"item.move\""))}}, {q("type")}, true)); o.insert(q("minItems"), 1); return o; }()},
                      {q("label"), js::str(QStringLiteral("History label (default: \"ops.apply\")"))}},
                     {q("ops")}),
             [](CallContext& c, const QJsonObject& a) {
               const std::vector<Op> ops = parseOps(a.value(q("ops")).toArray());
               return applyToSession(c, ops, a.value(q("label")).toString(QStringLiteral("ops.apply")));
             }));

  e.add(spec("clip.add", "addClip", "editor", true,
             QStringLiteral("Add a video/image asset to the timeline. Defaults: appended after the last item of the first compatible unlocked track; the whole asset (images: 5 s)."),
             js::obj({{q("assetId"), js::str(QStringLiteral("Asset id from assets.list"))}, {q("trackId"), js::str()}, {q("startFrame"), kFrame},
                      {q("durationFrames"), js::integer(QStringLiteral("Trim to this length"), 1)}, {q("sourceInFrame"), js::integer(QStringLiteral("Start partway into the asset"), 0)}},
                     {q("assetId")}),
             [](CallContext& c, const QJsonObject& a) { return addMedia(c, a, false); }));
  e.add(spec("audio.add", "addAudio", "editor", true, QStringLiteral("Add an audio asset (music, SFX, voiceover) to the first unlocked audio track (default at frame 0)."),
             js::obj({{q("assetId"), js::str()}, {q("trackId"), js::str()}, {q("startFrame"), kFrame}, {q("durationFrames"), js::integer(QString(), 1)},
                      {q("sourceInFrame"), js::integer(QString(), 0)}, {q("volume"), js::number(QStringLiteral("Linear gain, 1 = unity"), 0, 16)}},
                     {q("assetId")}),
             [](CallContext& c, const QJsonObject& a) { return addMedia(c, a, true); }));
  e.add(spec("text.add", "addText", "editor", true, QStringLiteral("Add a text item (title, callout) on the text track."),
             js::obj({{q("text"), [] { QJsonObject o = js::str(); o.insert(q("minLength"), 1); o.insert(q("maxLength"), 500); return o; }()}, {q("startFrame"), kFrame},
                      {q("durationFrames"), js::integer(QString(), 1)}, {q("trackId"), js::str()}, {q("style"), kTextStyleSchema},
                      {q("x"), js::number(QStringLiteral("Offset from centre in canvas px"))}, {q("y"), js::number()}},
                     {q("text"), q("startFrame"), q("durationFrames")}),
             addText));
  e.add(spec("shape.add", "", "editor", true, QStringLiteral("Add a shape (rect | ellipse | triangle) with fill, optional stroke, size within the canvas, and position offset."),
             js::obj({{q("shape"), js::choice({q("rect"), q("ellipse"), q("triangle")})}, {q("fill"), js::str()}, {q("stroke"), js::str()}, {q("strokeWidth"), js::number(QString(), 0)},
                      {q("radius"), js::number(QString(), 0)}, {q("width"), js::number(QString(), 1)}, {q("height"), js::number(QString(), 1)}, {q("startFrame"), kFrame},
                      {q("durationFrames"), js::integer(QString(), 1)}, {q("trackId"), js::str()}, {q("x"), js::number()}, {q("y"), js::number()}, {q("opacity"), js::number(QString(), 0, 1)}},
                     {q("startFrame"), q("durationFrames")}),
             addShape));
  e.add(spec("captions.add", "addCaptions", "editor", true,
             QStringLiteral("Add caption cards. Give `words` ([{w, startMs, endMs}] in timeline milliseconds) or the `assetId` of an asset that has a transcript. preset: karaoke (default, active "
                            "word highlighted), bold, serif."),
             js::obj({{q("words"), js::array(js::obj({{q("w"), js::str()}, {q("startMs"), js::integer(QString(), 0)}, {q("endMs"), js::integer(QString(), 0)}}, {q("w"), q("startMs"), q("endMs")}))},
                      {q("assetId"), js::str()}, {q("preset"), js::choice({q("karaoke"), q("bold"), q("serif")})}, {q("wordsPerCard"), js::integer(QString(), 1, 20)},
                      {q("fontSize"), js::number(QString(), 1)}, {q("color"), js::str()}, {q("placementY"), js::number(QString(), 0, 1)}, {q("trackId"), js::str()}}),
             addCaptions));
  e.add(spec("track.add", "", "editor", true, QStringLiteral("Add a track (video | audio | overlay | text)."),
             js::obj({{q("kind"), js::choice({q("video"), q("audio"), q("overlay"), q("text")})}, {q("name"), js::str()}, {q("index"), js::integer(QStringLiteral("0 = top; default bottom"), 0)}}, {q("kind")}),
             [](CallContext& c, const QJsonObject& a) {
               QJsonObject op{{q("type"), q("track.add")}, {q("trackId"), newId(q("trk"))}, {q("kind"), a.value(q("kind"))},
                              {q("name"), a.value(q("name")).toString(QStringLiteral("%1 %2").arg(a.value(q("kind")).toString()).arg(c.session->project->doc().tracks.size() + 1))}};
               if (a.contains(q("index"))) op.insert(q("index"), a.value(q("index")));
               return applyToSession(c, parseOps({op}), QStringLiteral("track.add"));
             }));
  e.add(spec("marker.add", "addMarker", "editor", true, QStringLiteral("Drop a labelled marker on the timeline ruler."),
             js::obj({{q("frame"), kFrame}, {q("label"), js::str()}, {q("color"), js::str()}}, {q("frame"), q("label")}),
             [](CallContext& c, const QJsonObject& a) {
               QJsonObject m{{q("id"), newId(q("mrk"))}, {q("frame"), a.value(q("frame"))}, {q("label"), a.value(q("label"))}};
               if (a.contains(q("color"))) m.insert(q("color"), a.value(q("color")));
               return applyToSession(c, parseOps({QJsonObject{{q("type"), q("marker.add")}, {q("marker"), m}}}), QStringLiteral("addMarker"), {{q("marker"), m}});
             }));

  e.add(spec("item.update", "updateItem", "editor", true,
             QStringLiteral("Change one item with a sparse patch: startFrame, durationFrames, sourceInFrame, speed, volume, muted, transform {x,y,scale,scaleX,scaleY,rotation,opacity} (merges), "
                            "labels {name,color}, effects, masks, keyframes, or `props` (whole-replace of the type-specific props)."),
             js::obj({{q("itemId"), js::str()}, {q("patch"), js::any(QStringLiteral("Fields to change"))}}, {q("itemId"), q("patch")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("itemId")).toString();
               return applyAndShow(c, parseOps({QJsonObject{{q("type"), q("item.update")}, {q("itemId"), id}, {q("patch"), a.value(q("patch"))}}}), QStringLiteral("updateItem"), {id});
             }));
  e.add(spec("item.move", "moveItem", "editor", true, QStringLiteral("Move an item to another position and/or compatible track."),
             js::obj({{q("itemId"), js::str()}, {q("trackId"), js::str()}, {q("startFrame"), kFrame}}, {q("itemId")}),
             [](CallContext& c, const QJsonObject& a) {
               QJsonObject op{{q("type"), q("item.move")}, {q("itemId"), a.value(q("itemId"))}};
               for (const char* k : {"trackId", "startFrame"}) if (a.contains(q(k))) op.insert(q(k), a.value(q(k)));
               return applyAndShow(c, parseOps({op}), QStringLiteral("moveItem"), {a.value(q("itemId")).toString()});
             }));
  e.add(spec("item.trim", "trimItem", "editor", true,
             QStringLiteral("Trim one edge of an item to a timeline frame. edge \"in\" keeps the end fixed; \"out\" keeps the start fixed. ripple slides downstream items."),
             js::obj({{q("itemId"), js::str()}, {q("edge"), js::choice({q("in"), q("out")})}, {q("frame"), kFrame}, {q("ripple"), js::boolean()}}, {q("itemId"), q("edge"), q("frame")}),
             [](CallContext& c, const QJsonObject& a) {
               const QJsonObject op{{q("type"), q("item.trim")}, {q("itemId"), a.value(q("itemId"))}, {q("edge"), a.value(q("edge"))}, {q("frame"), a.value(q("frame"))},
                                    {q("ripple"), a.value(q("ripple")).toBool(false)}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("trimItem"), {a.value(q("itemId")).toString()});
             }));
  e.add(spec("item.split", "splitItem", "editor", true, QStringLiteral("Split an item at a timeline frame into two independent items. Returns both halves (the new one's id is in `created.items`)."),
             js::obj({{q("itemId"), js::str()}, {q("atFrame"), kFrame}}, {q("itemId"), q("atFrame")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString nid = newId(q("itm"));
               const QJsonObject op{{q("type"), q("item.split")}, {q("itemId"), a.value(q("itemId"))}, {q("atFrame"), a.value(q("atFrame"))}, {q("newItemId"), nid}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("splitItem"), {a.value(q("itemId")).toString(), nid});
             }));
  e.add(spec("item.clone", "cloneItem", "editor", true, QStringLiteral("Duplicate an item (placed right after the original unless trackId/startFrame are given)."),
             js::obj({{q("itemId"), js::str()}, {q("trackId"), js::str()}, {q("startFrame"), kFrame}}, {q("itemId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString nid = newId(q("itm"));
               QJsonObject op{{q("type"), q("item.clone")}, {q("itemId"), a.value(q("itemId"))}, {q("newItemId"), nid}};
               for (const char* k : {"trackId", "startFrame"}) if (a.contains(q(k))) op.insert(q(k), a.value(q(k)));
               return applyAndShow(c, parseOps({op}), QStringLiteral("cloneItem"), {nid});
             }));
  e.add(spec("item.delete", "deleteItems", "editor", true, QStringLiteral("Remove items. ripple=true closes the gap."),
             js::obj({{q("itemIds"), [] { QJsonObject o = js::array(js::str()); o.insert(q("minItems"), 1); return o; }()}, {q("ripple"), js::boolean()}}, {q("itemIds")}),
             [](CallContext& c, const QJsonObject& a) {
               const QJsonObject op{{q("type"), q("item.remove")}, {q("itemIds"), a.value(q("itemIds"))}, {q("ripple"), a.value(q("ripple")).toBool(false)}};
               return applyToSession(c, parseOps({op}), QStringLiteral("deleteItems"), {{q("removed"), a.value(q("itemIds")).toArray().size()}});
             }));
  e.add(spec("item.slip", "slipItem", "editor", true, QStringLiteral("Shift which part of the source plays without moving the item (media items)."),
             js::obj({{q("itemId"), js::str()}, {q("sourceInFrame"), js::integer(QString(), 0)}}, {q("itemId"), q("sourceInFrame")}),
             [](CallContext& c, const QJsonObject& a) {
               const QJsonObject op{{q("type"), q("item.slip")}, {q("itemId"), a.value(q("itemId"))}, {q("sourceInFrame"), a.value(q("sourceInFrame"))}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("slipItem"), {a.value(q("itemId")).toString()});
             }));
  e.add(spec("item.speed.set", "", "editor", true, QStringLiteral("Set a media item's playback speed (0.1-16; 2 = twice as fast, the item keeps its source range and shortens)."),
             js::obj({{q("itemId"), js::str()}, {q("speed"), js::number(QString(), 0.1, 16)}}, {q("itemId"), q("speed")}),
             [](CallContext& c, const QJsonObject& a) {
               const QJsonObject op{{q("type"), q("item.setSpeed")}, {q("itemId"), a.value(q("itemId"))}, {q("speed"), a.value(q("speed"))}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("setSpeed"), {a.value(q("itemId")).toString()});
             }));
  e.add(spec("item.keyframes.set", "setKeyframes", "editor", true,
             QStringLiteral("Animate a numeric property: the full keyframe list for one property path (transform.scale, transform.opacity, transform.x, transform.y, transform.rotation, volume)."),
             js::obj({{q("itemId"), js::str()}, {q("property"), js::str()},
                      {q("keyframes"), js::array(js::obj({{q("frame"), kFrame}, {q("value"), js::number()}, {q("easing"), js::choice({q("linear"), q("hold"), q("easeIn"), q("easeOut"), q("easeInOut")})}},
                                                         {q("frame"), q("value")}))}},
                     {q("itemId"), q("property"), q("keyframes")}),
             [](CallContext& c, const QJsonObject& a) {
               const QJsonObject op{{q("type"), q("item.setKeyframes")}, {q("itemId"), a.value(q("itemId"))}, {q("property"), a.value(q("property"))}, {q("keyframes"), a.value(q("keyframes"))}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("setKeyframes"), {a.value(q("itemId")).toString()});
             }));

  // ---- colour ----
  e.add(spec("item.color.set", "", "colour", true,
             QStringLiteral("Colour inputs of one item (native-only): inputSpace (OpenColorIO colour space the source pixels are in, e.g. \"ARRI LogC3 (EI800)\"), lutPath (.cube/.3dl/.clf LUT applied "
                            "before inputSpace is interpreted) and lutIntensity (0..1). Fields not given keep their value; `clear` removes the block. See project.color.set for the project side."),
             js::obj({{q("itemId"), js::str()}, {q("inputSpace"), js::str()}, {q("lutPath"), js::str()}, {q("lutIntensity"), js::number(QString(), 0, 1)}, {q("clear"), js::boolean()}}, {q("itemId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("itemId")).toString();
               const Item* it = getItem(c.session->project->doc(), id);
               if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
               QJsonObject color;
               if (!a.value(q("clear")).toBool()) {
                 ItemColor cur = it->color.value_or(ItemColor{});
                 if (a.contains(q("inputSpace"))) cur.inputSpace = a.value(q("inputSpace")).toString();
                 if (a.contains(q("lutPath"))) cur.lutPath = a.value(q("lutPath")).toString();
                 if (a.contains(q("lutIntensity"))) cur.lutIntensity = a.value(q("lutIntensity")).toDouble();
                 if (cur.inputSpace) color.insert(q("inputSpace"), *cur.inputSpace);
                 if (cur.lutPath) color.insert(q("lutPath"), *cur.lutPath);
                 color.insert(q("lutIntensity"), cur.lutIntensity);
               }
               const QJsonObject op{{q("type"), q("item.update")}, {q("itemId"), id}, {q("patch"), QJsonObject{{q("color"), color}}}};
               return applyAndShow(c, parseOps({op}), QStringLiteral("item.color.set"), {id});
             }));

  // ---- audio ----
  e.add(spec("item.audio.set", "setClipProps", "audio", true,
             QStringLiteral("Clip audio: volume (linear, 1 = normal, 0 = silent), muted, fadeInFrames, fadeOutFrames (video and audio items)."),
             js::obj({{q("itemId"), js::str()}, {q("volume"), js::number(QString(), 0, 16)}, {q("muted"), js::boolean()}, {q("fadeInFrames"), js::integer(QString(), 0)},
                      {q("fadeOutFrames"), js::integer(QString(), 0)}},
                     {q("itemId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("itemId")).toString();
               const Item* it = getItem(c.session->project->doc(), id);
               if (!it) return notFound(QStringLiteral("Item %1 not found.").arg(id), QStringLiteral("Call timeline.get."));
               QJsonObject patch;
               if (a.contains(q("volume"))) patch.insert(q("volume"), a.value(q("volume")));
               if (a.contains(q("muted"))) patch.insert(q("muted"), a.value(q("muted")));
               if (a.contains(q("fadeInFrames")) || a.contains(q("fadeOutFrames"))) {
                 const FadeProps* fp = std::get_if<VideoProps>(&it->props) ? static_cast<const FadeProps*>(std::get_if<VideoProps>(&it->props))
                                       : std::get_if<AudioProps>(&it->props) ? static_cast<const FadeProps*>(std::get_if<AudioProps>(&it->props)) : nullptr;
                 if (!fp) return invalid(QStringLiteral("Fades apply to video and audio items; %1 is %2.").arg(id, enumName(it->type())));
                 patch.insert(q("props"), QJsonObject{{q("fadeInFrames"), a.contains(q("fadeInFrames")) ? a.value(q("fadeInFrames")) : QJsonValue(static_cast<double>(fp->fadeInFrames))},
                                                       {q("fadeOutFrames"), a.contains(q("fadeOutFrames")) ? a.value(q("fadeOutFrames")) : QJsonValue(static_cast<double>(fp->fadeOutFrames))}});
               }
               if (patch.isEmpty()) return invalid(QStringLiteral("Nothing to change: pass volume, muted, fadeInFrames and/or fadeOutFrames."));
               return applyAndShow(c, parseOps({QJsonObject{{q("type"), q("item.update")}, {q("itemId"), id}, {q("patch"), patch}}}), QStringLiteral("item.audio.set"), {id});
             }));
  e.add(spec("mixer.get", "", "audio", false, QStringLiteral("The audio mixer block (track strips, buses, master, inserts, sends, automation), or null when the project has none (every track at unity to master)."),
             js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               const auto& m = c.session->project->doc().mixer;
               return Result::success(QJsonObject{{q("mixer"), m ? QJsonValue(toJson(*m)) : QJsonValue(QJsonValue::Null)}});
             }));
  e.add(spec("mixer.set", "", "audio", true, QStringLiteral("Replace the whole mixer block (null removes it). One undo step. See mixer.node.set for single-node edits."),
             js::obj({{q("mixer"), js::anything(QStringLiteral("Mixer JSON ({strips, buses, master}) or null"))}}, {q("mixer")}),
             [](CallContext& c, const QJsonObject& a) {
               return applyToSession(c, parseOps({QJsonObject{{q("type"), q("mixer.set")}, {q("mixer"), a.value(q("mixer"))}}}), QStringLiteral("mixer.set"));
             }));
  e.add(spec("mixer.node.set", "", "audio", true,
             QStringLiteral("Edit one mixer node: a track strip (nodeId = track id, created if missing), a bus (bus id) or \"master\": volume (linear), pan (-1..1), muted, solo, output (bus id or null for master)."),
             js::obj({{q("nodeId"), js::str()}, {q("volume"), js::number(QString(), 0, 16)}, {q("pan"), js::number(QString(), -1, 1)}, {q("muted"), js::boolean()}, {q("solo"), js::boolean()},
                      {q("output"), [] { QJsonObject o = js::anything(QStringLiteral("Bus id, or null for the master")); return o; }()}},
                     {q("nodeId")}),
             [](CallContext& c, const QJsonObject& a) {
               const TimelineDoc& d = c.session->project->doc();
               Mixer m = d.mixer.value_or(Mixer{});
               const QString id = a.value(q("nodeId")).toString();
               MixNode* n = findNode(m, id);
               if (!n) {
                 if (!getTrack(d, id)) return notFound(QStringLiteral("No mixer node or track \"%1\".").arg(id), QStringLiteral("Use a track id, a bus id from mixer.get, or \"master\"."));
                 m.strips.push_back(MixNode{.id = id});
                 n = &m.strips.back();
               }
               if (a.contains(q("volume"))) n->volume = a.value(q("volume")).toDouble();
               if (a.contains(q("pan"))) n->pan = a.value(q("pan")).toDouble();
               if (a.contains(q("muted"))) n->muted = a.value(q("muted")).toBool();
               if (a.contains(q("solo"))) n->solo = a.value(q("solo")).toBool();
               if (a.contains(q("output"))) {
                 if (a.value(q("output")).isNull()) n->output.reset();
                 else n->output = a.value(q("output")).toString();
               }
               return applyToSession(c, {Op(MixerSet{m})}, QStringLiteral("mixer.node.set"), {{q("node"), id}});
             }));

  // ---- history ----
  const QJsonObject steps = js::integer(QStringLiteral("How many history entries"), 1, 50);
  e.add(spec("history.undo", "undo", "editor", true, QStringLiteral("Undo the last N history entries (default 1). One entry = one edit command or user edit."), js::obj({{q("steps"), steps}}),
             [](CallContext& c, const QJsonObject& a) { return historyStep(c, a, true); }));
  e.add(spec("history.redo", "redo", "editor", true, QStringLiteral("Redo the last N undone entries (default 1). Only until a new edit is made."), js::obj({{q("steps"), steps}}),
             [](CallContext& c, const QJsonObject& a) { return historyStep(c, a, false); }));
}

} // namespace sf::engine
