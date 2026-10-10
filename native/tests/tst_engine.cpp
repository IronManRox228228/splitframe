// The headless engine: every command family on a fixture project, batch atomicity, undo/redo with origin,
// registry introspection and errors as values.

#include "core/schema.h"
#include "engine/engine.h"
#include "media_testutil.h"

#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::engine;
using namespace sf::test;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }
} // namespace

class TstEngine : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString clip_, music_;
  std::unique_ptr<Engine> e_;

  QJsonObject ok(const QString& cmd, const QJsonObject& args = {}) {
    const Result r = e_->call(cmd, args);
    if (!r.ok) qWarning().noquote() << cmd << "failed:" << r.error.code << r.error.message;
    return r.ok ? r.value.toObject() : QJsonObject{{Q("__failed"), true}};
  }
  Result raw(const QString& cmd, const QJsonObject& args = {}) { return e_->call(cmd, args); }
  QJsonArray items(const QString& trackKind = {}) {
    Q_UNUSED(trackKind);
    QJsonArray all;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("mode"), Q("summary")}}).value(Q("tracks")).toArray())
      for (const QJsonValue& i : t.toObject().value(Q("items")).toArray()) all.append(i);
    return all;
  }
  QString firstAsset(const QString& kind) {
    for (const QJsonValue& a : ok(Q("assets.list")).value(Q("assets")).toArray())
      if (a.toObject().value(Q("kind")).toString() == kind) return a.toObject().value(Q("id")).toString();
    return {};
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    clip_ = dir_.filePath(QStringLiteral("clip.mp4"));
    music_ = dir_.filePath(QStringLiteral("music.wav"));
    QString log;
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 2), Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=440:sample_rate=48000:duration=2"), Q("-c:v"), Q("libx264"),
                        Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), Q("-c:a"), Q("aac"), Q("-shortest"), clip_}, &log), qPrintable(log));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=220:sample_rate=48000:duration=3"), music_}, &log), qPrintable(log));
  }

  void init() {
    e_ = std::make_unique<Engine>();
    ok(Q("project.new"), {{Q("name"), Q("Fixture")}, {Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}});
  }
  void cleanup() { e_.reset(); }

  // ---- registry ----
  void registryIsIntrospectable() {
    const QJsonArray cmds = e_->describe();
    QVERIFY(cmds.size() > 50);
    QSet<QString> names;
    int mutating = 0;
    for (const QJsonValue& v : cmds) {
      const QJsonObject c = v.toObject();
      QVERIFY2(!names.contains(c.value(Q("name")).toString()), "duplicate command name");
      names.insert(c.value(Q("name")).toString());
      QVERIFY2(QStringList({Q("editor"), Q("colour"), Q("audio"), Q("engine")}).contains(c.value(Q("domain")).toString()), qPrintable(c.value(Q("name")).toString()));
      QVERIFY(!c.value(Q("description")).toString().isEmpty());
      const QJsonObject schema = c.value(Q("inputSchema")).toObject();
      QCOMPARE(schema.value(Q("type")).toString(), Q("object"));
      const QJsonObject props = schema.value(Q("properties")).toObject();
      QVERIFY(props.contains(Q("session")));
      QCOMPARE(props.contains(Q("origin")), c.value(Q("mutates")).toBool()); // only mutating commands take an origin
      if (c.value(Q("mutates")).toBool()) ++mutating;
    }
    QVERIFY(mutating > 15);
    for (const char* n : {"timeline.get", "ops.apply", "clip.add", "item.split", "media.import", "frame.render", "export.start", "mixer.set", "item.color.set", "audio.analyze", "history.undo"})
      QVERIFY2(names.contains(Q(n)), n);
    QCOMPARE(e_->describe(Q("colour")).size() > 0, true);
    for (const QJsonValue& v : e_->describe(Q("audio"))) QCOMPARE(v.toObject().value(Q("domain")).toString(), Q("audio"));
    // Electron aliases and "_" spelling resolve
    QVERIFY(e_->find(Q("getTimeline")));
    QCOMPARE(e_->find(Q("timeline_get"))->name, Q("timeline.get"));
    QCOMPARE(e_->find(Q("splitItem"))->name, Q("item.split"));
    QVERIFY(ok(Q("engine.commands"), {{Q("domain"), Q("audio")}}).value(Q("commands")).toArray().size() > 0);
  }

  void errorsAreValues() {
    Result r = raw(Q("no.such.command"));
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("unknown_command"));
    r = raw(Q("clip.add"), {});
    QCOMPARE(r.error.code, Q("invalid_params"));
    QVERIFY2(r.error.message.contains(Q("assetId")), qPrintable(r.error.message));
    r = raw(Q("clip.add"), {{Q("assetId"), 5}});
    QCOMPARE(r.error.code, Q("invalid_params"));
    r = raw(Q("item.trim"), {{Q("itemId"), Q("x")}, {Q("edge"), Q("sideways")}, {Q("frame"), 1}});
    QCOMPARE(r.error.code, Q("invalid_params"));
    r = raw(Q("clip.add"), {{Q("assetId"), Q("ast_nope")}, {Q("bogus"), 1}});
    QCOMPARE(r.error.code, Q("invalid_params")); // unknown property
    r = raw(Q("clip.add"), {{Q("assetId"), Q("ast_nope")}});
    QCOMPARE(r.error.code, Q("not_found"));
    r = raw(Q("item.get"), {{Q("itemId"), Q("itm_nope")}});
    QCOMPARE(r.error.code, Q("not_found"));
    r = raw(Q("timeline.get"), {{Q("session"), Q("s99")}});
    QCOMPARE(r.error.code, Q("no_session"));
    r = raw(Q("project.open"), {{Q("path"), dir_.filePath(Q("missing.json"))}});
    QCOMPARE(r.error.code, Q("not_found"));
    // bad op JSON: SchemaError becomes invalid_params, not an exception
    r = raw(Q("ops.apply"), {{Q("ops"), QJsonArray{QJsonObject{{Q("type"), Q("item.move")}, {Q("itemId"), 5}}}}});
    QCOMPARE(r.error.code, Q("invalid_params"));
    QVERIFY(r.error.message.contains(Q("ops[0]")));
    Engine empty;
    QCOMPARE(empty.call(Q("timeline.get")).error.code, Q("no_session"));
    const QJsonObject asJson = r.toJson();
    QCOMPARE(asJson.value(Q("ok")).toBool(), false);
    QVERIFY(asJson.value(Q("error")).toObject().contains(Q("message")));
  }

  // ---- session ----
  void sessionLifecycle() {
    QJsonObject info = ok(Q("project.info"));
    QCOMPARE(info.value(Q("name")).toString(), Q("Fixture"));
    QCOMPARE(info.value(Q("width")).toInt(), 320);
    QCOMPARE(info.value(Q("fps")).toInt(), 25);
    ok(Q("marker.add"), {{Q("frame"), 3}, {Q("label"), Q("m")}});
    QVERIFY(ok(Q("project.info")).value(Q("dirty")).toBool());
    QCOMPARE(raw(Q("project.save")).error.code, Q("invalid_params")); // no file yet
    const QString path = dir_.filePath(Q("saved.json"));
    QVERIFY(!ok(Q("project.saveAs"), {{Q("path"), path}}).contains(Q("__failed")));
    QVERIFY(QFileInfo::exists(path));
    QVERIFY(!ok(Q("project.info")).value(Q("dirty")).toBool());
    ok(Q("marker.add"), {{Q("frame"), 4}, {Q("label"), Q("m2")}});
    const QString first = e_->currentSession();
    const QJsonObject second = ok(Q("project.open"), {{Q("path"), path}});
    QVERIFY(second.value(Q("session")).toString() != first);
    QCOMPARE(second.value(Q("markers")).toInt(), 1);
    QCOMPARE(e_->sessionIds().size(), 2);
    QCOMPARE(ok(Q("sessions.list")).value(Q("sessions")).toArray().size(), 2);
    QCOMPARE(raw(Q("project.close"), {{Q("session"), first}}).error.code, Q("busy")); // unsaved
    QVERIFY(raw(Q("project.close"), {{Q("session"), first}, {Q("discard"), true}}).ok);
    QCOMPARE(e_->sessionIds().size(), 1);
    // reset a session in place
    ok(Q("project.new"), {{Q("session"), second.value(Q("session")).toString()}, {Q("name"), Q("Again")}});
    QCOMPARE(ok(Q("project.info")).value(Q("name")).toString(), Q("Again"));
  }

  void projectSettings() {
    ok(Q("marker.add"), {{Q("frame"), 3}, {Q("label"), Q("m")}});
    QJsonObject r = ok(Q("project.settings.set"), {{Q("aspect"), Q("9:16")}, {Q("shortSidePx"), 360}, {Q("fps"), 30}, {Q("name"), Q("Vertical")}});
    QVERIFY(r.value(Q("applied")).toInt() >= 3);
    QJsonObject info = ok(Q("project.info"));
    QCOMPARE(info.value(Q("width")).toInt(), 360);
    QCOMPARE(info.value(Q("height")).toInt(), 640);
    QCOMPARE(info.value(Q("fps")).toInt(), 30);
    QCOMPARE(info.value(Q("name")).toString(), Q("Vertical"));
    QCOMPARE(raw(Q("project.settings.set"), {{Q("width"), 640}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("project.settings.set"), {}).error.code, Q("invalid_params"));
    ok(Q("history.undo"));
    QCOMPARE(ok(Q("project.info")).value(Q("width")).toInt(), 320);
    // colour management (native-only block)
    ok(Q("project.color.set"), {{Q("outputSpace"), Q("rec709")}, {Q("blendSpace"), Q("display")}});
    const QJsonObject cm = ok(Q("project.info")).value(Q("colorManagement")).toObject();
    QCOMPARE(cm.value(Q("outputSpace")).toString(), Q("rec709"));
    QCOMPARE(cm.value(Q("blendSpace")).toString(), Q("display"));
    ok(Q("project.color.set"), {{Q("clear"), true}});
    QVERIFY(!ok(Q("project.info")).contains(Q("colorManagement")));
  }

  // ---- media + edit + read ----
  void mediaAndEditing() {
    // import
    QJsonObject imp = ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_, dir_.filePath(Q("nope.mp4"))}}});
    QCOMPARE(imp.value(Q("added")).toArray().size(), 2);
    QCOMPARE(imp.value(Q("missing")).toArray().size(), 1);
    QVERIFY(imp.value(Q("analysisComplete")).toBool());
    QCOMPARE(ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}}).value(Q("duplicates")).toArray().size(), 1);
    const QString video = firstAsset(Q("video")), audio = firstAsset(Q("audio"));
    QVERIFY(!video.isEmpty() && !audio.isEmpty());
    const QJsonObject probe = ok(Q("asset.probe"), {{Q("assetId"), video}});
    QCOMPARE(probe.value(Q("width")).toInt(), 320);
    QVERIFY(ok(Q("asset.probe"), {{Q("path"), clip_}}).value(Q("hasAudio")).toBool());
    QCOMPARE(raw(Q("asset.probe"), {{Q("path"), dir_.filePath(Q("nope.mp4"))}}).error.code, Q("not_found"));
    QCOMPARE(ok(Q("assets.list"), {{Q("kind"), Q("audio")}}).value(Q("assets")).toArray().size(), 1);

    QSignalSpy events(e_.get(), &Engine::event);
    // clip.add: whole asset on the main track
    QJsonObject added = ok(Q("clip.add"), {{Q("assetId"), video}, {Q("origin"), Q("agent:editor")}});
    const QString clipId = added.value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    QVERIFY(!clipId.isEmpty());
    QCOMPARE(added.value(Q("items")).toArray().at(0).toObject().value(Q("durationFrames")).toInt(), 50);
    QCOMPARE(added.value(Q("inverse")).toArray().at(0).toObject().value(Q("type")).toString(), Q("item.remove"));
    ok(Q("audio.add"), {{Q("assetId"), audio}, {Q("durationFrames"), 40}, {Q("volume"), 0.5}});
    QCOMPARE(raw(Q("audio.add"), {{Q("assetId"), video}}).error.code, Q("invalid_params"));
    // text / shape / marker / captions
    ok(Q("text.add"), {{Q("text"), Q("Hello")}, {Q("startFrame"), 5}, {Q("durationFrames"), 20}, {Q("style"), QJsonObject{{Q("fontSize"), 40}}}});
    ok(Q("shape.add"), {{Q("shape"), Q("ellipse")}, {Q("fill"), Q("#ff0000")}, {Q("startFrame"), 0}, {Q("durationFrames"), 10}});
    ok(Q("marker.add"), {{Q("frame"), 12}, {Q("label"), Q("beat")}});
    const QJsonObject caps = ok(Q("captions.add"), {{Q("words"), QJsonArray{QJsonObject{{Q("w"), Q("hello")}, {Q("startMs"), 0}, {Q("endMs"), 400}},
                                                                            QJsonObject{{Q("w"), Q("world")}, {Q("startMs"), 500}, {Q("endMs"), 900}}}}, {Q("wordsPerCard"), 1}});
    QCOMPARE(caps.value(Q("captionCards")).toInt(), 2);
    ok(Q("track.add"), {{Q("kind"), Q("video")}, {Q("name"), Q("B-roll")}, {Q("index"), 0}});

    // read back
    QJsonObject tl = ok(Q("timeline.get"));
    QCOMPARE(tl.value(Q("project")).toObject().value(Q("fps")).toInt(), 25);
    QCOMPARE(tl.value(Q("markers")).toArray().size(), 1);
    QCOMPARE(items().size(), 6);
    QCOMPARE(ok(Q("timeline.get"), {{Q("itemTypes"), QJsonArray{Q("caption")}}}).value(Q("tracks")).toArray().size() > 0, true);
    int captionsOnly = 0;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("itemTypes"), QJsonArray{Q("caption")}}}).value(Q("tracks")).toArray()) captionsOnly += t.toObject().value(Q("items")).toArray().size();
    QCOMPARE(captionsOnly, 2);
    int inRange = 0;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("startFrame"), 30}, {Q("endFrame"), 45}}).value(Q("tracks")).toArray()) inRange += t.toObject().value(Q("items")).toArray().size();
    QCOMPARE(inRange, 2); // the video clip (0..50) and the audio clip (0..40)
    QVERIFY(ok(Q("timeline.get"), {{Q("mode"), Q("detail")}}).value(Q("tracks")).toArray().at(0).toObject().contains(Q("items")));
    QCOMPARE(raw(Q("timeline.get"), {{Q("trackIds"), QJsonArray{Q("trk_nope")}}}).error.code, Q("not_found"));
    const QJsonObject full = ok(Q("item.get"), {{Q("itemId"), clipId}});
    QCOMPARE(full.value(Q("assetId")).toString(), video);
    QVERIFY(full.contains(Q("transform")));

    // split / move / trim / speed / clone / slip / keyframes / update
    const QJsonObject split = ok(Q("item.split"), {{Q("itemId"), clipId}, {Q("atFrame"), 20}});
    const QString rightId = split.value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    QCOMPARE(split.value(Q("items")).toArray().size(), 2);
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), clipId}}).value(Q("durationFrames")).toInt(), 20);
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("startFrame")).toInt(), 20);
    ok(Q("item.trim"), {{Q("itemId"), rightId}, {Q("edge"), Q("in")}, {Q("frame"), 25}});
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("startFrame")).toInt(), 25);
    ok(Q("item.move"), {{Q("itemId"), rightId}, {Q("startFrame"), 60}});
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("startFrame")).toInt(), 60);
    const int durBefore = ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("durationFrames")).toInt();
    ok(Q("item.speed.set"), {{Q("itemId"), rightId}, {Q("speed"), 2}});
    QVERIFY(ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("durationFrames")).toInt() < durBefore || ok(Q("item.get"), {{Q("itemId"), rightId}}).value(Q("speed")).toDouble() == 2.0);
    QCOMPARE(raw(Q("item.speed.set"), {{Q("itemId"), rightId}, {Q("speed"), 99}}).error.code, Q("invalid_params"));
    const QJsonObject clone = ok(Q("item.clone"), {{Q("itemId"), clipId}});
    QCOMPARE(clone.value(Q("created")).toObject().value(Q("items")).toArray().size(), 1);
    ok(Q("item.slip"), {{Q("itemId"), clipId}, {Q("sourceInFrame"), 3}});
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), clipId}}).value(Q("sourceInFrame")).toInt(), 3);
    ok(Q("item.keyframes.set"), {{Q("itemId"), clipId}, {Q("property"), Q("transform.opacity")}, {Q("keyframes"), QJsonArray{QJsonObject{{Q("frame"), 0}, {Q("value"), 0}}, QJsonObject{{Q("frame"), 10}, {Q("value"), 1}}}}});
    ok(Q("item.update"), {{Q("itemId"), clipId}, {Q("patch"), QJsonObject{{Q("transform"), QJsonObject{{Q("scale"), 0.5}}}, {Q("labels"), QJsonObject{{Q("name"), Q("renamed")}}}}}});
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), clipId}}).value(Q("transform")).toObject().value(Q("scale")).toDouble(), 0.5);
    QCOMPARE(raw(Q("item.move"), {{Q("itemId"), Q("itm_nope")}, {Q("startFrame"), 1}}).error.code, Q("edit_rejected"));

    // colour + audio facade
    ok(Q("item.color.set"), {{Q("itemId"), clipId}, {Q("inputSpace"), Q("ACEScg")}, {Q("lutIntensity"), 0.5}});
    QJsonObject color = ok(Q("item.get"), {{Q("itemId"), clipId}}).value(Q("color")).toObject();
    QCOMPARE(color.value(Q("inputSpace")).toString(), Q("ACEScg"));
    QCOMPARE(color.value(Q("lutIntensity")).toDouble(), 0.5);
    ok(Q("item.color.set"), {{Q("itemId"), clipId}, {Q("clear"), true}});
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), clipId}}).contains(Q("color")));
    ok(Q("item.audio.set"), {{Q("itemId"), clipId}, {Q("volume"), 0.7}, {Q("fadeInFrames"), 5}});
    const QJsonObject withFade = ok(Q("item.get"), {{Q("itemId"), clipId}});
    QCOMPARE(withFade.value(Q("volume")).toDouble(), 0.7);
    QCOMPARE(withFade.value(Q("props")).toObject().value(Q("fadeInFrames")).toInt(), 5);
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    ok(Q("mixer.node.set"), {{Q("nodeId"), Q("master")}, {Q("volume"), 0.8}});
    const QString audioTrack = [&] {
      for (const QJsonValue& t : ok(Q("timeline.get")).value(Q("tracks")).toArray())
        if (t.toObject().value(Q("kind")).toString() == Q("audio")) return t.toObject().value(Q("id")).toString();
      return QString();
    }();
    ok(Q("mixer.node.set"), {{Q("nodeId"), audioTrack}, {Q("pan"), -0.5}, {Q("solo"), true}});
    const QJsonObject mixer = ok(Q("mixer.get")).value(Q("mixer")).toObject();
    QCOMPARE(mixer.value(Q("master")).toObject().value(Q("volume")).toDouble(), 0.8);
    QCOMPARE(mixer.value(Q("strips")).toArray().at(0).toObject().value(Q("pan")).toDouble(), -0.5);
    QCOMPARE(raw(Q("mixer.node.set"), {{Q("nodeId"), Q("trk_nope")}, {Q("volume"), 1}}).error.code, Q("not_found"));
    ok(Q("mixer.set"), {{Q("mixer"), QJsonValue(QJsonValue::Null)}});
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());

    // selection + delete
    ok(Q("selection.set"), {{Q("ids"), QJsonArray{clipId, Q("itm_ghost")}}});
    QCOMPARE(ok(Q("selection.get")).value(Q("selection")).toArray().size(), 1);
    const int before = items().size();
    ok(Q("item.delete"), {{Q("itemIds"), QJsonArray{rightId}}, {Q("ripple"), true}});
    QCOMPARE(items().size(), before - 1);

    // events: every mutation emitted ops.applied carrying the origin
    int applied = 0;
    bool sawOrigin = false;
    for (const auto& args : std::as_const(events))
      if (args.at(0).toString() == Q("ops.applied")) {
        ++applied;
        sawOrigin = sawOrigin || args.at(1).toJsonObject().value(Q("actor")).toString() == Q("agent:editor");
      }
    QVERIFY(applied >= 20);
    QVERIFY(sawOrigin);

    // media.remove: refused while used, then with removeClips
    QCOMPARE(raw(Q("media.remove"), {{Q("assetId"), video}}).error.code, Q("edit_rejected"));
    QVERIFY(raw(Q("media.remove"), {{Q("assetId"), video}, {Q("removeClips"), true}}).ok);
    QVERIFY(firstAsset(Q("video")).isEmpty());
    // relink: a second copy of the audio file under another name
    const QString copy = dir_.filePath(Q("music_copy.wav"));
    QVERIFY(QFile::copy(music_, copy));
    const QJsonObject relinked = ok(Q("media.relink"), {{Q("assetId"), audio}, {Q("path"), copy}});
    QCOMPARE(relinked.value(Q("status")).toString(), Q("analyzed"));
    QVERIFY(relinked.value(Q("path")).toString().endsWith(Q("music_copy.wav")));
  }

  void captionsFromTranscript() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    ok(Q("clip.add"), {{Q("assetId"), video}});
    QCOMPARE(raw(Q("captions.add"), {{Q("assetId"), video}}).error.code, Q("not_found")); // no transcript, and a helpful message
    QCOMPARE(raw(Q("captions.add"), {}).error.code, Q("invalid_params"));
  }

  // ---- atomicity, history, origin ----
  void batchIsAtomic() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    ok(Q("clip.add"), {{Q("assetId"), video}});
    const QJsonObject before = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    const QJsonObject histBefore = ok(Q("history.list"));
    const QString clipId = items().at(0).toObject().value(Q("id")).toString();
    const QJsonArray ops{QJsonObject{{Q("type"), Q("marker.add")}, {Q("marker"), QJsonObject{{Q("id"), Q("mrk_0123456789abcdef")}, {Q("frame"), 1}, {Q("label"), Q("a")}}}},
                         QJsonObject{{Q("type"), Q("item.move")}, {Q("itemId"), clipId}, {Q("startFrame"), 5}},
                         QJsonObject{{Q("type"), Q("item.move")}, {Q("itemId"), Q("itm_missing")}, {Q("startFrame"), 5}}};
    const Result r = raw(Q("ops.apply"), {{Q("ops"), ops}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("edit_rejected"));
    QCOMPARE(ok(Q("timeline.get"), {{Q("mode"), Q("detail")}}), before); // nothing changed, not even the first two ops
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), histBefore.value(Q("total")).toInt());
    // and the same batch without the bad op applies as ONE undo step
    const QJsonObject good = ok(Q("ops.apply"), {{Q("ops"), QJsonArray{ops.at(0), ops.at(1)}}, {Q("label"), Q("two ops")}});
    QCOMPARE(good.value(Q("applied")).toInt(), 2);
    QCOMPARE(good.value(Q("inverse")).toArray().size(), 2);
    QCOMPARE(good.value(Q("created")).toObject().value(Q("markers")).toArray().at(0).toString(), Q("mrk_0123456789abcdef"));
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(ok(Q("timeline.get"), {{Q("mode"), Q("detail")}}), before);
  }

  void undoRedoAndOrigin() {
    ok(Q("marker.add"), {{Q("frame"), 1}, {Q("label"), Q("user-ish")}});
    ok(Q("ops.apply"), {{Q("ops"), QJsonArray{QJsonObject{{Q("type"), Q("project.rename")}, {Q("name"), Q("By agent")}}}}, {Q("label"), Q("rename")}, {Q("origin"), Q("agent:editor")}});
    QCOMPARE(ok(Q("project.info")).value(Q("name")).toString(), Q("By agent"));
    QJsonObject hist = ok(Q("history.list"));
    QCOMPARE(hist.value(Q("total")).toInt(), 2);
    const QJsonArray entries = hist.value(Q("entries")).toArray();
    QCOMPARE(entries.at(0).toObject().value(Q("actor")).toString(), Q("api")); // no origin: default
    QCOMPARE(entries.at(1).toObject().value(Q("actor")).toString(), Q("agent:editor"));
    QCOMPARE(entries.at(1).toObject().value(Q("label")).toString(), Q("rename"));
    QVERIFY(entries.at(1).toObject().value(Q("applied")).toBool());
    QJsonObject u = ok(Q("history.undo"));
    QCOMPARE(u.value(Q("undone")).toArray().at(0).toObject().value(Q("actor")).toString(), Q("agent:editor"));
    QCOMPARE(ok(Q("project.info")).value(Q("name")).toString(), Q("Fixture"));
    QVERIFY(ok(Q("history.list")).value(Q("canRedo")).toBool());
    QVERIFY(!ok(Q("history.list")).value(Q("entries")).toArray().at(1).toObject().value(Q("applied")).toBool());
    QCOMPARE(ok(Q("history.redo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(ok(Q("project.info")).value(Q("name")).toString(), Q("By agent"));
    QCOMPARE(ok(Q("history.undo"), {{Q("steps"), 5}}).value(Q("steps")).toInt(), 2); // only two exist
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 0);
    QVERIFY(ok(Q("history.undo")).value(Q("note")).toString().contains(Q("Nothing")));
    // undo/redo emit doc.changed
    QSignalSpy spy(e_.get(), &Engine::event);
    ok(Q("history.redo"));
    bool changed = false;
    for (const auto& a : std::as_const(spy)) changed = changed || a.at(0).toString() == Q("doc.changed");
    QVERIFY(changed);
  }

  // ---- render / audio ----
  void renderAndAudio() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_}}});
    const QString video = firstAsset(Q("video")), audio = firstAsset(Q("audio"));
    ok(Q("clip.add"), {{Q("assetId"), video}});
    ok(Q("audio.add"), {{Q("assetId"), audio}, {Q("durationFrames"), 50}});
    ok(Q("text.add"), {{Q("text"), Q("Hi")}, {Q("startFrame"), 0}, {Q("durationFrames"), 25}});

    const Result r = raw(Q("frame.render"), {{Q("frame"), 10}, {Q("inline"), true}});
    if (!r.ok && r.error.code == Q("unavailable") && r.error.message.contains(Q("renderer"))) QSKIP("no D3D11 device for the offscreen renderer");
    QVERIFY2(r.ok, qPrintable(r.error.message));
    const QJsonObject fr = r.value.toObject();
    QCOMPARE(fr.value(Q("width")).toInt(), 320);
    QCOMPARE(fr.value(Q("height")).toInt(), 180);
    QVERIFY(QFileInfo::exists(fr.value(Q("path")).toString()) || !fr.contains(Q("path")));
    QImage png;
    QVERIFY(png.loadFromData(QByteArray::fromBase64(fr.value(Q("base64")).toString().toLatin1()), "PNG"));
    QCOMPARE(png.size(), QSize(320, 180));
    // size cap
    const QJsonObject small = ok(Q("frame.render"), {{Q("frame"), 0}, {Q("maxWidth"), 160}, {Q("path"), dir_.filePath(Q("small.png"))}});
    QCOMPARE(small.value(Q("width")).toInt(), 160);
    QCOMPARE(QImage(dir_.filePath(Q("small.png"))).size(), QSize(160, 90));
    QCOMPARE(raw(Q("frame.render"), {{Q("frame"), -1}}).error.code, Q("invalid_params"));

    const QJsonObject loud = ok(Q("audio.analyze"), {{Q("targetLufs"), -14}});
    QVERIFY(!loud.value(Q("silent")).toBool());
    QVERIFY(loud.value(Q("integratedLufs")).toDouble() < 0 && loud.value(Q("integratedLufs")).toDouble() > -60);
    QVERIFY(loud.contains(Q("suggestedGainDb")));
    QCOMPARE(raw(Q("audio.analyze"), {{Q("startFrame"), 10}, {Q("endFrame"), 5}}).error.code, Q("invalid_params"));

    const QJsonObject wave = ok(Q("waveform.get"), {{Q("assetId"), audio}, {Q("buckets"), 50}});
    QVERIFY(wave.value(Q("buckets")).toInt() > 0 && wave.value(Q("buckets")).toInt() <= 100);
    QCOMPARE(wave.value(Q("min")).toArray().size(), wave.value(Q("max")).toArray().size());
    QCOMPARE(raw(Q("waveform.get"), {{Q("assetId"), Q("ast_nope")}}).error.code, Q("not_found"));
    const QJsonObject th = ok(Q("thumbnail.get"), {{Q("assetId"), video}, {Q("atSec"), 0.5}, {Q("inline"), true}});
    QVERIFY(th.value(Q("width")).toInt() > 0 && th.value(Q("width")).toInt() <= 320);
    QCOMPARE(raw(Q("thumbnail.get"), {{Q("assetId"), audio}}).error.code, Q("invalid_params"));
  }

  void proxies() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    const QJsonObject r = ok(Q("proxies.request"), {{Q("assetId"), video}, {Q("wait"), true}, {Q("timeoutMs"), 60000}});
    QVERIFY(r.value(Q("complete")).toBool());
    QCOMPARE(r.value(Q("proxies")).toArray().at(0).toObject().value(Q("state")).toString(), Q("unneeded")); // 320 px wide: no proxy needed
    QCOMPARE(ok(Q("proxies.status")).value(Q("proxies")).toArray().size(), 1);
    QCOMPARE(raw(Q("proxies.request"), {}).error.code, Q("invalid_params"));
  }

  // ---- export ----
  void exportJob() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    ok(Q("clip.add"), {{Q("assetId"), firstAsset(Q("video"))}});
    QCOMPARE(ok(Q("presets.list")).value(Q("presets")).toArray().size() > 3, true);
    QCOMPARE(raw(Q("export.start"), {{Q("out"), dir_.filePath(Q("x.mp4"))}, {Q("codec"), Q("h264")}, {Q("width"), 101}}).error.code, Q("invalid_params")); // odd size
    QCOMPARE(raw(Q("export.status"), {{Q("jobId"), Q("exp_nope")}}).error.code, Q("not_found"));

    QSignalSpy spy(e_.get(), &Engine::event);
    const QString out = dir_.filePath(Q("engine.mp4"));
    const QJsonObject st = ok(Q("export.start"), {{Q("out"), out}, {Q("quality"), Q("draft")}, {Q("range"), QJsonArray{0, 10}}});
    const QString job = st.value(Q("jobId")).toString();
    QVERIFY(!job.isEmpty());
    QCOMPARE(st.value(Q("plan")).toObject().value(Q("frames")).toInt(), 10);
    const QJsonObject done = ok(Q("export.status"), {{Q("jobId"), job}, {Q("waitMs"), 90000}});
    QCOMPARE(done.value(Q("state")).toString(), Q("done"));
    QCOMPARE(done.value(Q("result")).toObject().value(Q("videoFrames")).toInt(), 10);
    QVERIFY(QFileInfo(out).size() > 1000);
    bool finished = false;
    for (const auto& a : std::as_const(spy)) finished = finished || a.at(0).toString() == Q("export.finished");
    QVERIFY(finished);
    // existing file needs overwrite
    QCOMPARE(raw(Q("export.start"), {{Q("out"), out}}).error.code, Q("invalid_params"));
    // cancel
    const QJsonObject st2 = ok(Q("export.start"), {{Q("out"), dir_.filePath(Q("cancel.mp4"))}, {Q("quality"), Q("high")}});
    ok(Q("export.cancel"), {{Q("jobId"), st2.value(Q("jobId")).toString()}});
    const QString s2 = ok(Q("export.status"), {{Q("jobId"), st2.value(Q("jobId")).toString()}, {Q("waitMs"), 90000}}).value(Q("state")).toString();
    QVERIFY2(s2 == Q("cancelled") || s2 == Q("done"), qPrintable(s2)); // a 50-frame job may beat the cancel
    QCOMPARE(ok(Q("export.status")).value(Q("jobs")).toArray().size(), 2);
  }
};

QTEST_MAIN(TstEngine)
#include "tst_engine.moc"
