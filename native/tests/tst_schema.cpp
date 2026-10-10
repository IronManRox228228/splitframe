#include "test_helpers.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

// Native-only coverage of the schema layer (the TS side exercises zod through every other test):
// defaults on load, path-first validation errors, round trips, files and op parsing.
using namespace sf;
using namespace sf::test;

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }

QJsonValue json(const char* text) {
  QJsonParseError err;
  const QJsonDocument d = QJsonDocument::fromJson(QByteArray(text), &err);
  if (err.error != QJsonParseError::NoError) qFatal("bad test json: %s", qPrintable(err.errorString()));
  return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
}

// A sparse doc: only what zod requires, so every default must come from the schema.
const char* kMinimalDoc = R"({
  "project": {"id": "prj_0123456789abcdef", "name": "Min", "createdAt": "t0", "updatedAt": "t1"},
  "tracks": [{"id": "trk_a", "kind": "video", "name": "V"}],
  "items": [{
    "id": "itm_a", "trackId": "trk_a", "type": "video", "startFrame": 5, "durationFrames": 10,
    "assetId": "ast_x",
    "transform": {"x": 0, "y": 0, "scale": 1, "scaleX": 1, "scaleY": 1, "rotation": 0, "opacity": 1},
    "props": {}, "somethingNew": 1
  }],
  "markers": []
})";

// Parses a doc, applies a mutation to the JSON first, and returns the SchemaError path.
QString errorPath(const std::function<void(QJsonObject&)>& mutate) {
  QJsonObject root = json(kMinimalDoc).toObject();
  mutate(root);
  try {
    parseTimelineDoc(Rd(root));
  } catch (const SchemaError& e) {
    return e.path();
  }
  return QStringLiteral("<no error>");
}

QJsonObject firstItem(const QJsonObject& root) { return root.value(QStringLiteral("items")).toArray().at(0).toObject(); }
void setFirstItem(QJsonObject& root, const QJsonObject& item) { root.insert(QStringLiteral("items"), QJsonArray{item}); }

const char* kJson0 = R"({"words": [{"w": "hi", "startMs": 0, "endMs": 5}],
                    "style": {"fontFamily": "Inter", "fontSize": 40, "color": "#fff"}})";
const char* kJson1 = R"({"project": 5})";
const char* kJson2 = R"({"shape": "star", "fill": "#000", "width": 1, "height": 1})";
const char* kJson3 = R"({"type": "item.move", "itemId": 5})";
const char* kJson4 = R"({"type": "batch", "ops": [{"type": "item.trim", "itemId": "a", "edge": "mid", "frame": 1}]})";
const char* kJson5 = R"({"type": "nope"})";
const char* kJson6 = R"({
      "doc": {"project": {"id": "prj_1", "name": "B", "createdAt": "a", "updatedAt": "b"}, "tracks": [], "items": [], "markers": []},
      "assets": [{"id": "ast_1", "projectId": "prj_1", "kind": "video", "path": "C:/a.mp4", "originalName": "a.mp4",
                  "createdAt": "c", "fps": 29.97, "metadata": {"codec": "h264"}}],
      "transcripts": [{"assetId": "ast_1", "words": [{"w": "x", "startMs": 1, "endMs": 2}]}],
      "scenes": [{"id": "scn_1", "assetId": "ast_1", "startMs": 0, "endMs": 10, "description": "d"}],
      "beatMaps": [{"assetId": "ast_1", "bpm": 120.5, "beatsMs": [0, 500], "downbeatsMs": [0],
                    "sections": [{"startMs": 0, "endMs": 10, "label": "intro"}]}]
    })";

} // namespace

class TstSchema : public QObject {
  Q_OBJECT
private slots:
  void appliesZodDefaultsOnLoad() {
    const TimelineDoc d = parseTimelineDoc(Rd(json(kMinimalDoc)));
    QCOMPARE_EQ(d.project.fps, 30);
    QCOMPARE_EQ(d.project.width, 1920);
    QCOMPARE_EQ(d.project.height, 1080);
    QCOMPARE(d.project.styleConfig.primaryColor, S("#fbbf24"));
    QCOMPARE(d.project.styleConfig.backgroundColor, S("#0a0a0a"));
    QVERIFY(d.project.styleConfig.fonts.empty());
    QVERIFY(!d.tracks[0].locked && !d.tracks[0].muted && !d.tracks[0].hidden);
    const Item& it = d.items[0];
    QCOMPARE_EQ(it.speed, 1);
    QCOMPARE_EQ(it.volume, 1);
    QVERIFY(!it.muted);
    QVERIFY(it.timeRemap.empty() && it.effects.empty() && it.masks.empty() && it.keyframes.empty());
    QVERIFY(it.labels == Labels{});
    const auto& p = std::get<VideoProps>(it.props);
    QCOMPARE_EQ(p.fadeInFrames, 0);
    QCOMPARE_EQ(p.fadeOutFrames, 0);
  }

  void dropsUnknownFieldsLikeZodStrip() {
    const TimelineDoc d = parseTimelineDoc(Rd(json(kMinimalDoc)));
    const QJsonObject out = toJson(d.items[0]);
    QVERIFY(!out.contains(QStringLiteral("somethingNew")));
    // and the output is itself a fixed point of parse + write
    const TimelineDoc again = parseTimelineDoc(Rd(toJson(d)));
    QVERIFY(again == d);
  }

  void textAndCaptionStyleDefaults() {
    const ItemProps p = parseItemProps(
        ItemType::Caption,
        Rd(json(kJson0)));
    const auto& c = std::get<CaptionProps>(p);
    QCOMPARE(enumName(c.mode), S("phrase"));
    QCOMPARE_EQ(c.maxWordsPerCard, 5);
    QCOMPARE_EQ(c.style.fontWeight, 700);
    QCOMPARE(enumName(c.style.align), S("center"));
    QCOMPARE_EQ(c.style.lineHeight, 1.2);
    QCOMPARE(enumName(c.style.highlight), S("active-word"));
    QCOMPARE(c.style.highlightColor, S("#fbbf24"));
    QCOMPARE_EQ(c.style.placementY, 0.82);
    QCOMPARE_EQ(c.style.maxCharsPerLine, 28);
  }

  void errorsNameTheBadFieldPath_data() {
    QTest::addColumn<QString>("expected");
    QTest::addColumn<int>("which");
    QTest::newRow("missing project name") << S("project.name") << 0;
    QTest::newRow("float fps") << S("project.fps") << 1;
    QTest::newRow("bad track kind") << S("tracks[0].kind") << 2;
    QTest::newRow("zero duration") << S("items[0].durationFrames") << 3;
    QTest::newRow("negative start") << S("items[0].startFrame") << 4;
    QTest::newRow("speed out of range") << S("items[0].speed") << 5;
    QTest::newRow("zero scale") << S("items[0].transform.scale") << 6;
    QTest::newRow("bad discriminator") << S("items[0].type") << 7;
    QTest::newRow("bad keyframe easing") << S("items[0].keyframes.transform.x[0].easing") << 8;
    QTest::newRow("opacity above 1") << S("items[0].transform.opacity") << 9;
    QTest::newRow("items not an array") << S("items") << 10;
    QTest::newRow("null is not optional") << S("items[0].assetId") << 11;
  }
  void errorsNameTheBadFieldPath() {
    QFETCH(QString, expected);
    QFETCH(int, which);
    const QString got = errorPath([&](QJsonObject& root) {
      switch (which) {
        case 0: {
          QJsonObject p = root.value(QStringLiteral("project")).toObject();
          p.remove(QStringLiteral("name"));
          root.insert(QStringLiteral("project"), p);
          break;
        }
        case 1: {
          QJsonObject p = root.value(QStringLiteral("project")).toObject();
          p.insert(QStringLiteral("fps"), 29.97);
          root.insert(QStringLiteral("project"), p);
          break;
        }
        case 2: {
          root.insert(QStringLiteral("tracks"), QJsonArray{QJsonObject{{"id", "t"}, {"kind", "sideways"}, {"name", "x"}}});
          break;
        }
        case 3: { QJsonObject i = firstItem(root); i.insert(QStringLiteral("durationFrames"), 0); setFirstItem(root, i); break; }
        case 4: { QJsonObject i = firstItem(root); i.insert(QStringLiteral("startFrame"), -1); setFirstItem(root, i); break; }
        case 5: { QJsonObject i = firstItem(root); i.insert(QStringLiteral("speed"), 20); setFirstItem(root, i); break; }
        case 6: {
          QJsonObject i = firstItem(root);
          QJsonObject t = i.value(QStringLiteral("transform")).toObject();
          t.insert(QStringLiteral("scale"), 0);
          i.insert(QStringLiteral("transform"), t);
          setFirstItem(root, i);
          break;
        }
        case 7: { QJsonObject i = firstItem(root); i.insert(QStringLiteral("type"), QStringLiteral("hologram")); setFirstItem(root, i); break; }
        case 8: {
          QJsonObject i = firstItem(root);
          i.insert(QStringLiteral("keyframes"),
                   QJsonObject{{"transform.x", QJsonArray{QJsonObject{{"frame", 0}, {"value", 1}, {"easing", "bouncy"}}}}});
          setFirstItem(root, i);
          break;
        }
        case 9: {
          QJsonObject i = firstItem(root);
          QJsonObject t = i.value(QStringLiteral("transform")).toObject();
          t.insert(QStringLiteral("opacity"), 1.5);
          i.insert(QStringLiteral("transform"), t);
          setFirstItem(root, i);
          break;
        }
        case 10: root.insert(QStringLiteral("items"), QStringLiteral("nope")); break;
        default: { QJsonObject i = firstItem(root); i.insert(QStringLiteral("assetId"), QJsonValue::Null); setFirstItem(root, i); break; }
      }
    });
    QCOMPARE(got, expected);
  }

  void messagesAreReadable() {
    try {
      parseTimelineDoc(Rd(json(kJson1)));
      QFAIL("expected SchemaError");
    } catch (const SchemaError& e) {
      QCOMPARE(QString::fromUtf8(e.what()), S("project: Expected object, received number"));
    }
    try {
      parseItemProps(ItemType::Shape, Rd(json(kJson2), S("props")));
      QFAIL("expected SchemaError");
    } catch (const SchemaError& e) {
      QCOMPARE(e.path(), S("props.shape"));
      QVERIFY(e.issue().contains(S("'rect' | 'ellipse' | 'triangle'")));
    }
  }

  void invalidJsonTextReportsTheOffset() {
    SF_THROWS_MATCH(parseTimelineDocJson("{\"project\": "), "Invalid JSON");
  }

  void idsHaveTheSchemaFormat() {
    const QString id = newId(S("itm"));
    QVERIFY(isValidId(S("itm"), id));
    QVERIFY(!isValidId(S("trk"), id));
    QVERIFY(!isValidId(S("itm"), S("itm_xyz")));
    QVERIFY(!isValidId(S("itm"), S("itm_0123")));
    QCOMPARE_EQ(id.size(), 4 + 16);
    QVERIFY(newId(S("itm")) != newId(S("itm")));
  }

  void createItemValidates() {
    const TimelineDoc doc = makeDoc();
    ItemInit init;
    init.id = S("itm_1");
    init.trackId = trackOfKind(doc, TrackKind::Video).id;
    init.startFrame = 0;
    init.durationFrames = 10;
    SF_THROWS_MATCH(createItem(ItemType::Video, init), "require an assetId");
    init.assetId = S("ast_v");
    init.speed = 99;
    SF_THROWS(createItem(ItemType::Video, init), SchemaError);
    init.speed = 1;
    init.durationFrames = 0;
    SF_THROWS(createItem(ItemType::Video, init), SchemaError);
    init.durationFrames = 10;
    QCOMPARE_EQ(createItem(ItemType::Video, init).type() == ItemType::Video, true);
    // text needs its props
    init.assetId.reset();
    SF_THROWS_MATCH(createItem(ItemType::Text, init), "props.text");
  }

  void opsAreValidatedBeforeApply() {
    const TimelineDoc doc = makeDoc();
    SF_THROWS(applyOp(doc, ProjectRename{QString()}), SchemaError);
    SF_THROWS(applyOp(doc, ProjectSetFps{0}), SchemaError);
    SF_THROWS(applyOp(doc, ProjectSetFps{121}), SchemaError);
    SF_THROWS(applyOp(doc, ItemRemove{{}, false}), SchemaError);
    SF_THROWS(applyOp(doc, BatchOp{}), SchemaError);
    SF_THROWS(applyOp(doc, ItemSetSpeed{S("itm_x"), 0.01}), SchemaError);
  }

  void itemAddWithoutPropsFillsDefaultsOrFails() {
    const TimelineDoc doc = makeDoc();
    // video: defaults
    ItemAdd v;
    v.item = videoItem(doc, 0, 10);
    v.propsOmitted = true;
    const auto r = applyOp(doc, v);
    QVERIFY(std::holds_alternative<VideoProps>(r.doc.items[0].props));
    QVERIFY(!toJson(Op(v)).value(QStringLiteral("item")).toObject().contains(QStringLiteral("props")));
    // text: zod's itemPropsSchemas.text.parse({}) throws
    ItemAdd t;
    t.item = textItem(doc, 0, 10);
    t.propsOmitted = true;
    SF_THROWS(applyOp(doc, t), SchemaError);
  }

  void fpsChangeRescalesAndRestoreInverts() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 30, 60, [](ItemInit& i) {
      i.keyframes = KeyframeMap{{S("transform.x"), {Keyframe{15, 1, Easing::Linear}}}};
    });
    TimelineDoc d1 = docWith(doc, {item});
    d1 = applyOp(d1, MarkerAdd{Marker{S("mrk_1"), 90, S("m"), std::nullopt}}).doc;
    const auto r = applyOp(d1, ProjectSetFps{60});
    QCOMPARE_EQ(r.doc.project.fps, 60);
    QCOMPARE_EQ(r.doc.items[0].startFrame, 60);
    QCOMPARE_EQ(r.doc.items[0].durationFrames, 120);
    QCOMPARE_EQ(r.doc.items[0].keyframes.at(S("transform.x"))[0].frame, 30);
    QCOMPARE_EQ(r.doc.markers[0].frame, 180);
    const TimelineDoc back = applyOps(r.doc, r.inverse).doc;
    SF_COMPARE(back.items, d1.items);
    QCOMPARE_EQ(back.project.fps, 30);
  }

  void everyOpKindSurvivesAJsonRoundTrip() {
    const TimelineDoc doc = makeDoc();
    const Item item = videoItem(doc, 0, 90);
    Effect fx{S("fx1"), S("blur"), {{S("amount"), 3.5}, {S("mode"), S("fast")}, {S("on"), true}}};
    Mask mask;
    mask.id = S("m1");
    mask.shape = MaskShape::Path;
    mask.path = std::vector<Point>{{0, 0}, {10, 0}, {10, 10}};
    mask.tracking = MaskTracking{S("ast_t"), TrackingStatus::Done, {{1, 2, 3}}};
    ItemUpdate up;
    up.itemId = item.id;
    up.patch.volume = 0.5;
    up.patch.labels = LabelsPatch{std::optional<QString>(S("n")), std::nullopt};
    up.patch.props = QJsonValue(QJsonObject{{"fadeInFrames", 3}});
    EffectUpdate eu{item.id, S("fx1"), EffectPatch{S("glow"), EffectParams{{S("amount"), 1.0}}}};
    TrackAdd ta;
    ta.trackId = S("trk_new");
    ta.kind = TrackKind::Overlay;
    ta.name = S("O");
    ta.index = 1;
    ProjectSetReference refNull;
    const std::vector<Op> ops = {
        ProjectRename{S("n")}, ProjectSetCanvas{1280, 720}, ProjectSetFps{24},
        ProjectRestoreTimeline{30, {item}, {Marker{S("mrk_1"), 1, S("l"), S("#fff")}}},
        ProjectSetStyleConfig{StyleConfig{}}, ProjectSetReference{S("ast_r")}, refNull, ta, TrackRemove{S("trk_new")},
        TrackUpdate{S("trk_new"), TrackPatch{S("x"), true, std::nullopt, false}},
        TrackReorder{{S("a"), S("b")}}, addItem(item), ItemRemove{{S("a"), S("b")}, true}, up,
        ItemMove{S("a"), S("trk"), 5}, ItemTrim{S("a"), TrimEdge::Out, 7, true}, ItemSplit{S("a"), 9, S("b")},
        ItemClone{S("a"), S("b"), std::nullopt, 3}, ItemSlip{S("a"), 4}, ItemSetSpeed{S("a"), 2.5},
        ItemSetTimeRemap{S("a"), {{0, 0}, {5, 10}}},
        ItemSetKeyframes{S("a"), S("transform.x"), {Keyframe{1, 2, Easing::EaseInOut}}},
        EffectAdd{S("a"), fx}, EffectRemove{S("a"), S("fx1")}, eu, MaskAdd{S("a"), mask}, MaskRemove{S("a"), S("m1")},
        MarkerAdd{Marker{S("mrk_2"), 2, S("l"), std::nullopt}}, MarkerRemove{S("mrk_2")},
        MarkerUpdate{S("mrk_2"), MarkerPatch{3, S("q"), std::nullopt}},
        BatchOp{{ProjectRename{S("n")}, ItemSlip{S("a"), 4}}},
    };
    for (const Op& op : ops) {
      const QJsonObject j = toJson(op);
      const Op back = parseOp(Rd(j));
      QVERIFY2(toJson(back) == j, qPrintable(op.type()));
      QCOMPARE(back.type(), op.type());
    }
  }

  void opParseErrorsNameThePath() {
    try {
      parseOp(Rd(json(kJson3)));
      QFAIL("expected SchemaError");
    } catch (const SchemaError& e) {
      QCOMPARE(e.path(), S("itemId"));
    }
    try {
      parseOp(Rd(json(kJson4)));
      QFAIL("expected SchemaError");
    } catch (const SchemaError& e) {
      QCOMPARE(e.path(), S("ops[0].edge"));
    }
    SF_THROWS(parseOp(Rd(json(kJson5))), SchemaError);
  }

  void savesAndLoadsAFileWithoutLosingAnything() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(S("project.json"));
    TimelineDoc doc = makeDoc();
    CaptionProps cp;
    cp.words = {{S("hey"), 0, 400, 0.9, S("spk1")}};
    cp.style.fontFamily = S("Inter");
    cp.style.fontSize = 48;
    cp.style.color = S("#fff");
    cp.style.strokeColor = S("#000");
    ItemInit ci;
    ci.id = S("itm_cap");
    ci.trackId = trackOfKind(doc, TrackKind::Text).id;
    ci.durationFrames = 30;
    ci.props = cp;
    ItemInit mi;
    mi.id = S("itm_mg");
    mi.trackId = trackOfKind(doc, TrackKind::Text).id;
    mi.startFrame = 30;
    mi.durationFrames = 30;
    MotionGraphicProps mg;
    mg.code = S("export default () => null;");
    mg.inputProps = QJsonObject{{"title", "hi"}, {"n", QJsonArray{1, 2, 3}}};
    mi.props = mg;
    doc = docWith(doc, {createItem(ItemType::Caption, ci), createItem(ItemType::MotionGraphic, mi), videoItem(doc, 0, 90)});
    saveTimelineDoc(path, doc);
    const TimelineDoc loaded = loadTimelineDoc(path);
    QVERIFY(loaded == doc);
    // compact form too
    QVERIFY(parseTimelineDocJson(timelineDocToJson(doc)) == doc);
    SF_THROWS(loadTimelineDoc(dir.filePath(S("missing.json"))), SchemaError);
  }

  void projectBundleRoundTrips() {
    const char* text = kJson6;
    const ProjectBundle b = parseProjectBundle(Rd(json(text)));
    QCOMPARE(enumName(b.assets[0].status), S("importing"));
    QCOMPARE_EQ(b.assets[0].durationMs, 0);
    QVERIFY(!b.assets[0].hasAudio);
    QCOMPARE(b.transcripts[0].language, S("en"));
    QVERIFY(b.scenes[0].tags.empty());
    QCOMPARE_EQ(b.beatMaps[0].sections[0].energy, 0.5);
    QVERIFY(parseProjectBundleJson(projectBundleToJson(b)) == b);
  }

  // Native-only colour fields: absent stays absent, present round-trips, bad values name their path
  void colourManagementFieldsAreOptionalAndRoundTrip() {
    QJsonObject root = json(kMinimalDoc).toObject();
    TimelineDoc plain = parseTimelineDoc(Rd(root));
    QVERIFY(!plain.project.colorManagement);
    QVERIFY(!plain.items[0].color);
    QVERIFY(!toJson(plain)["project"].toObject().contains(S("colorManagement")));
    QVERIFY(!toJson(plain)["items"].toArray()[0].toObject().contains(S("color")));

    QJsonObject project = root["project"].toObject();
    project["colorManagement"] = json(R"({"workingSpace": "ACEScg", "displayView": "sRGB - Display/ACES 2.0", "blendSpace": "display"})");
    root["project"] = project;
    QJsonArray items = root["items"].toArray();
    QJsonObject item = items[0].toObject();
    item["color"] = json(R"x({"inputSpace": "ARRI LogC3 (EI800)", "lutPath": "C:/luts/look.cube", "lutIntensity": 0.5})x");
    items[0] = item;
    root["items"] = items;
    const TimelineDoc doc = parseTimelineDoc(Rd(root));
    QVERIFY(doc.project.colorManagement);
    QCOMPARE(*doc.project.colorManagement->workingSpace, S("ACEScg"));
    QVERIFY(!doc.project.colorManagement->outputSpace);
    QCOMPARE(doc.project.colorManagement->blendSpace, BlendSpace::Display);
    QCOMPARE(*doc.items[0].color->lutPath, S("C:/luts/look.cube"));
    QCOMPARE(doc.items[0].color->lutIntensity, 0.5);
    const TimelineDoc again = parseTimelineDoc(Rd(toJson(doc)));
    QCOMPARE(again, doc);

    // a colour block without blendSpace defaults to linear; bad values fail with their path
    project["colorManagement"] = json(R"({"workingSpace": "ACEScg"})");
    root["project"] = project;
    QCOMPARE(parseTimelineDoc(Rd(root)).project.colorManagement->blendSpace, BlendSpace::Linear);
    QVERIFY(errorPath([](QJsonObject& r) {
              QJsonObject p = r["project"].toObject();
              p["colorManagement"] = json(R"({"blendSpace": "gamma"})");
              r["project"] = p;
            }).contains(S("blendSpace")));
    QVERIFY(errorPath([](QJsonObject& r) {
              QJsonArray its = r["items"].toArray();
              QJsonObject it = its[0].toObject();
              it["color"] = json(R"({"lutIntensity": 2})");
              its[0] = it;
              r["items"] = its;
            }).contains(S("lutIntensity")));
  }
};

QTEST_APPLESS_MAIN(TstSchema)
#include "tst_schema.moc"
