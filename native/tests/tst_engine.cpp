// The headless engine: every command family on a fixture project, batch atomicity, undo/redo with origin,
// registry introspection and errors as values.

#include "core/schema.h"
#include "engine/engine.h"
#include "media_testutil.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace sf;
using namespace sf::engine;
using namespace sf::test;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }

// A 16-bit stereo 48 kHz WAV of drum-like hits (3 kHz burst, 8 ms decay) every 60/bpm seconds from `first`.
bool writeClickWav(const QString& path, double bpm, double seconds, double first = 0.13) {
  const int sr = 48000;
  const size_t n = static_cast<size_t>(seconds * sr);
  std::vector<qint16> pcm(n * 2, 0);
  for (double t0 = first; t0 < seconds - 0.1; t0 += 60.0 / bpm)
    for (int i = 0; i < 2400; ++i) {
      const size_t k = static_cast<size_t>(t0 * sr) + static_cast<size_t>(i);
      if (k >= n) break;
      const double t = i / static_cast<double>(sr);
      const qint16 x = static_cast<qint16>(16000.0 * std::exp(-t / 0.008) * std::sin(6.283185307179586 * 3000.0 * t));
      pcm[k * 2] = pcm[k * 2 + 1] = x;
    }
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  const quint32 dataBytes = static_cast<quint32>(pcm.size() * 2);
  const auto le32 = [&](quint32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  const auto le16 = [&](quint16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  f.write("RIFF");
  le32(36 + dataBytes);
  f.write("WAVEfmt ");
  le32(16);
  le16(1);
  le16(2);
  le32(sr);
  le32(sr * 4);
  le16(4);
  le16(16);
  f.write("data");
  le32(dataBytes);
  f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<qint64>(dataBytes));
  return true;
}

// Counts its calls and returns the asset path: the scheduler's genericity without any decoding.
class FakeAnalyzer final : public sf::jobs::Analyzer {
public:
  QString id() const override { return QStringLiteral("fake"); }
  int version() const override { return 1; }
  sf::jobs::AnalyzerResult run(sf::jobs::JobContext& ctx) override {
    ++calls;
    ctx.report(1.0);
    return sf::jobs::AnalyzerResult::success({{QStringLiteral("path"), ctx.path}});
  }
  std::atomic<int> calls{0};
};
} // namespace

class TstEngine : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString clip_, music_, story_, talk_, beats_, gray_, vert_;
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
    qputenv("SF_SYSTEM_DIR", dir_.filePath(QStringLiteral("sys")).toUtf8()); // the smoke-test cache and the overrides stay in the temp dir
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    clip_ = dir_.filePath(QStringLiteral("clip.mp4"));
    music_ = dir_.filePath(QStringLiteral("music.wav"));
    QString log;
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 2), Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=440:sample_rate=48000:duration=2"), Q("-c:v"), Q("libx264"),
                        Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), Q("-c:a"), Q("aac"), Q("-shortest"), clip_}, &log), qPrintable(log));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=220:sample_rate=48000:duration=3"), music_}, &log), qPrintable(log));
    // measured editing: a silent 10 s picture, a 10 s clip whose tone speaks in the second half of every 2 s, and a 100 BPM click track
    story_ = dir_.filePath(QStringLiteral("story.mp4"));
    talk_ = dir_.filePath(QStringLiteral("talk.mp4"));
    beats_ = dir_.filePath(QStringLiteral("beats.wav"));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 10), Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), story_}, &log), qPrintable(log));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(320, 180, 25, 10), Q("-f"), Q("lavfi"), Q("-i"), Q("sine=frequency=440:sample_rate=48000:duration=10,volume=volume=0:enable='lt(mod(t,2),1)'"),
                        Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), Q("-c:a"), Q("aac"), Q("-shortest"), talk_}, &log), qPrintable(log));
    QVERIFY(writeClickWav(beats_, 100.0, 20.0));
    vert_ = dir_.filePath(QStringLiteral("vert.mp4"));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), indexSource(180, 320, 25, 2), Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), vert_}, &log), qPrintable(log));
    // style library: a flat mid-grey picture (a neutral cast to measure a look against)
    gray_ = dir_.filePath(QStringLiteral("gray.mp4"));
    QVERIFY2(runFfmpeg({Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=0x808080:s=320x180:r=25:d=2"), Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"), gray_}, &log), qPrintable(log));
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
      QVERIFY2(QStringList({Q("editor"), Q("colour"), Q("audio"), Q("engine"), Q("comfy"), Q("style"), Q("generate")}).contains(c.value(Q("domain")).toString()), qPrintable(c.value(Q("name")).toString()));
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

    // A per-clip LUT must reach offscreen renders (frame grabs and exports), not just the preview.
    // This LUT removes blue entirely, so the graded frame's blue mean has to collapse.
    const auto blueMean = [&] {
      const QJsonObject f = ok(Q("frame.render"), {{Q("frame"), 10}, {Q("inline"), true}});
      QImage img;
      img.loadFromData(QByteArray::fromBase64(f.value(Q("base64")).toString().toLatin1()), "PNG");
      img = img.convertToFormat(QImage::Format_RGB32);
      double sum = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) sum += qBlue(img.pixel(x, y));
      return sum / (img.width() * img.height());
    };
    {
      QFile lut(dir_.filePath(Q("no_blue.cube")));
      QVERIFY(lut.open(QIODevice::WriteOnly | QIODevice::Text));
      QTextStream ts(&lut);
      const int n = 17;
      ts << "TITLE \"no_blue\"\nLUT_3D_SIZE " << n << "\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
      for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
          for (int red = 0; red < n; ++red)
            ts << QString::number(red / double(n - 1), 'f', 6) << ' ' << QString::number(g / double(n - 1), 'f', 6) << " 0.000000\n";
    }
    const double before = blueMean();
    QString clipItem;
    for (const QJsonValue v : items())
      if (v.toObject().value(Q("type")).toString() == Q("video")) clipItem = v.toObject().value(Q("id")).toString();
    QVERIFY(!clipItem.isEmpty());
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("lutPath"), dir_.filePath(Q("no_blue.cube"))}, {Q("lutIntensity"), 1.0}});
    const double after = blueMean();
    QVERIFY2(before > 20 && after < before * 0.25, qPrintable(Q("blue mean %1 -> %2").arg(before).arg(after)));
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("clear"), true}});

    // Primary correction reaches frame.render, merges partial updates, and each call is one undo step
    const double plainBlue = blueMean();
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("primaries"), QJsonObject{{Q("exposure"), -2.0}}}});
    const double darkBlue = blueMean();
    QVERIFY2(darkBlue < plainBlue * 0.8, qPrintable(Q("blue mean %1 -> %2").arg(plainBlue).arg(darkBlue)));
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("primaries"), QJsonObject{{Q("lift"), QJsonObject{{Q("master"), 0.1}}}, {Q("gain"), QJsonObject{{Q("r"), 1.5}}}}}});
    QJsonObject prim = ok(Q("item.get"), {{Q("itemId"), clipItem}}).value(Q("color")).toObject().value(Q("primaries")).toObject();
    QCOMPARE(prim.value(Q("exposure")).toDouble(), -2.0); // kept
    QCOMPARE(prim.value(Q("lift")).toObject().value(Q("master")).toDouble(), 0.1);
    QCOMPARE(prim.value(Q("gain")).toObject().value(Q("r")).toDouble(), 1.5);
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("primaries"), QJsonObject{{Q("lift"), QJsonObject{{Q("g"), 0.05}}}}}});
    prim = ok(Q("item.get"), {{Q("itemId"), clipItem}}).value(Q("color")).toObject().value(Q("primaries")).toObject();
    QCOMPARE(prim.value(Q("lift")).toObject().value(Q("master")).toDouble(), 0.1); // the channel merge keeps the master
    QCOMPARE(prim.value(Q("lift")).toObject().value(Q("g")).toDouble(), 0.05);
    ok(Q("history.undo"));
    ok(Q("history.undo"));
    QCOMPARE(blueMean(), darkBlue); // back to exposure -2 only
    ok(Q("history.undo"));
    QCOMPARE(blueMean(), plainBlue);
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), clipItem}}).contains(Q("color")));
    // out of range is rejected; resetPrimaries drops the primaries (and with them the whole, otherwise empty, block)
    QVERIFY(!raw(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("primaries"), QJsonObject{{Q("exposure"), 9.0}}}}).ok);
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("primaries"), QJsonObject{{Q("saturation"), 0.0}}}});
    ok(Q("item.color.set"), {{Q("itemId"), clipItem}, {Q("resetPrimaries"), true}});
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), clipItem}}).contains(Q("color")));

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

  // ---- colour measurement ----
  void colourMeasurementAndProposals() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    ok(Q("clip.add"), {{Q("assetId"), video}});
    ok(Q("clip.add"), {{Q("assetId"), video}});
    QStringList clips;
    for (const QJsonValue v : items())
      if (v.toObject().value(Q("type")).toString() == Q("video")) clips << v.toObject().value(Q("id")).toString();
    QCOMPARE(clips.size(), 2);

    const Result probe = raw(Q("colour.analyze"), {{Q("itemId"), clips[0]}, {Q("samples"), 3}});
    if (!probe.ok && probe.error.code == Q("unavailable") && probe.error.message.contains(Q("renderer"))) QSKIP("no D3D11 device for the offscreen renderer");
    QVERIFY2(probe.ok, qPrintable(probe.error.message));
    const QJsonObject m = probe.value.toObject();
    QVERIFY(m.value(Q("valid")).toBool());
    QCOMPARE(m.value(Q("frames")).toInt(), 3);
    QVERIFY(m.value(Q("pixels")).toDouble() > 1000);
    QCOMPARE(m.value(Q("histogram")).toObject().value(Q("fraction")).toArray().size(), 64);
    QVERIFY(m.value(Q("luma")).toObject().contains(Q("stopsVs18Grey")));
    QVERIFY(m.contains(Q("cast")) && m.contains(Q("skin")) && m.contains(Q("crushedPct")));
    QCOMPARE(raw(Q("colour.analyze"), {{Q("itemId"), clips[0]}, {Q("samples"), 3}}).value.toObject().value(Q("luma")).toObject(), m.value(Q("luma")).toObject()); // deterministic
    QVERIFY(ok(Q("colour.analyze"), {{Q("frame"), 5}}).value(Q("valid")).toBool());                                                                      // the composite
    QCOMPARE(raw(Q("colour.analyze"), {{Q("itemId"), Q("itm_nope")}}).error.code, Q("not_found"));

    // autoBalance: a proposal does nothing until apply
    const QJsonObject dry = ok(Q("colour.autoBalance"), {{Q("itemId"), clips[0]}, {Q("samples"), 2}});
    QVERIFY(!dry.value(Q("applied")).toBool());
    QVERIFY(dry.contains(Q("proposedPrimaries")) && dry.contains(Q("predicted")));
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), clips[0]}}).contains(Q("color")));
    // the dark -3 stop grade is something auto balance wants to lift: apply it, then undo
    ok(Q("item.color.set"), {{Q("itemId"), clips[0]}, {Q("primaries"), QJsonObject{{Q("exposure"), -3.0}}}});
    const QJsonObject applied = ok(Q("colour.autoBalance"), {{Q("itemId"), clips[0]}, {Q("samples"), 2}, {Q("apply"), true}, {Q("origin"), Q("agent:colourist")}});
    QVERIFY(applied.value(Q("applied")).toBool());
    const QJsonObject prim = ok(Q("item.get"), {{Q("itemId"), clips[0]}}).value(Q("color")).toObject().value(Q("primaries")).toObject();
    const QJsonObject dark{{Q("exposure"), -3.0}};
    QVERIFY2(prim != dark, "autoBalance replaced the -3 stop grade");
    QCOMPARE(applied.value(Q("proposedPrimaries")).toObject(), prim);
    ok(Q("history.undo"));
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), clips[0]}}).value(Q("color")).toObject().value(Q("primaries")).toObject().value(Q("exposure")).toDouble(), -3.0);
    ok(Q("history.undo"));
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), clips[0]}}).contains(Q("color")));

    // match: the same source graded dark matches back onto the plain one
    ok(Q("item.color.set"), {{Q("itemId"), clips[1]}, {Q("primaries"), QJsonObject{{Q("exposure"), -1.0}, {Q("temperature"), 0.2}}}});
    const QJsonObject mt = ok(Q("colour.match"), {{Q("itemId"), clips[1]}, {Q("referenceItemId"), clips[0]}, {Q("samples"), 2}});
    QVERIFY(mt.value(Q("residual")).toObject().value(Q("rmsStops")).toDouble() < 0.1);
    QVERIFY(mt.value(Q("plausible")).toBool()); // one stop and a little warmth is an ordinary match
    QVERIFY(!mt.value(Q("applied")).toBool());
    QCOMPARE(raw(Q("colour.match"), {{Q("itemId"), clips[0]}, {Q("referenceItemId"), clips[0]}}).error.code, Q("invalid_params"));

    // check: unchanged is ok, a -3 stop grade moves the median far past the default limit
    const QJsonObject same = ok(Q("colour.check"), {{Q("itemId"), clips[0]}, {Q("before"), m}, {Q("samples"), 3}});
    QVERIFY2(same.value(Q("ok")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(same).toJson())));
    ok(Q("item.color.set"), {{Q("itemId"), clips[0]}, {Q("primaries"), QJsonObject{{Q("exposure"), -3.0}}}});
    const QJsonObject bad = ok(Q("colour.check"), {{Q("itemId"), clips[0]}, {Q("before"), m}, {Q("samples"), 3}});
    QVERIFY2(!bad.value(Q("ok")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(bad).toJson())));
    QStringList codes;
    for (const QJsonValue v : bad.value(Q("violations")).toArray()) codes << v.toObject().value(Q("code")).toString();
    QVERIFY2(codes.contains(Q("exposureShiftStops")), qPrintable(codes.join(Q(","))));
    const QJsonObject lenient = ok(Q("colour.check"), {{Q("itemId"), clips[0]}, {Q("before"), m}, {Q("samples"), 3}, {Q("thresholds"), QJsonObject{{Q("exposureShiftStops"), 5.0}, {Q("blacksCrushedIncrease"), 100.0}}}});
    QVERIFY(lenient.value(Q("ok")).toBool());
    QCOMPARE(e_->describe(Q("colour")).size() > 3, true);
  }

  // ---- measured audio ----
  void measuredAudio() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_}}});
    ok(Q("clip.add"), {{Q("assetId"), firstAsset(Q("video"))}});
    ok(Q("audio.add"), {{Q("assetId"), firstAsset(Q("audio"))}});
    QString videoTrack, audioTrack, musicItem;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("mode"), Q("summary")}}).value(Q("tracks")).toArray()) {
      const QJsonObject to = t.toObject();
      if (to.value(Q("items")).toArray().isEmpty()) continue;
      const QString kind = to.value(Q("kind")).toString();
      if (kind == Q("video")) videoTrack = to.value(Q("id")).toString();
      if (kind == Q("audio")) {
        audioTrack = to.value(Q("id")).toString();
        musicItem = to.value(Q("items")).toArray().at(0).toObject().value(Q("id")).toString();
      }
    }
    QVERIFY(!videoTrack.isEmpty() && !audioTrack.isEmpty() && !musicItem.isEmpty());

    // measure: the master equals audio.analyze, tracks and the asset are measured alone
    const QJsonObject m = ok(Q("audio.measure"));
    const QJsonObject an = ok(Q("audio.analyze"));
    QVERIFY2(std::isfinite(m.value(Q("integratedLufs")).toDouble(1000)) && m.value(Q("integratedLufs")).toDouble(1000) < 0, qPrintable(QString::fromUtf8(QJsonDocument(m).toJson())));
    QCOMPARE(m.value(Q("integratedLufs")).toDouble(), an.value(Q("integratedLufs")).toDouble());
    QCOMPARE(m.value(Q("truePeakDbtp")).toDouble(), an.value(Q("truePeakDbtp")).toDouble());
    QCOMPARE(m.value(Q("clippedSamples")).toInt(), 0);
    QVERIFY(m.value(Q("shortTermLufs")).toObject().value(Q("values")).toArray().size() >= 3);
    QVERIFY(m.contains(Q("noiseFloorDb")) && m.contains(Q("clipRuns")));
    const QJsonObject mt = ok(Q("audio.measure"), {{Q("scope"), audioTrack}});
    QVERIFY(mt.value(Q("integratedLufs")).toDouble() < m.value(Q("integratedLufs")).toDouble() + 3.0);
    QCOMPARE(mt.value(Q("scope")).toObject().value(Q("kind")).toString(), Q("track"));
    QVERIFY(std::isfinite(ok(Q("audio.measure"), {{Q("scope"), firstAsset(Q("audio"))}}).value(Q("integratedLufs")).toDouble(1000)));
    QCOMPARE(raw(Q("audio.measure"), {{Q("scope"), Q("nope")}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("audio.measure"), {{Q("startFrame"), 10}, {Q("endFrame"), 5}}).error.code, Q("invalid_params"));

    // check before: the two tones sit at the same level, the music ends without a fade, the loudness is far from -14
    const QJsonObject c0 = ok(Q("audio.check"));
    QVERIFY(!c0.value(Q("pass")).toBool());
    QStringList rules0;
    for (const QJsonValue v : c0.value(Q("violations")).toArray()) rules0 << v.toObject().value(Q("rule")).toString();
    QVERIFY2(rules0.contains(Q("loudness")) && rules0.contains(Q("speech_margin")) && rules0.contains(Q("abrupt_end")), qPrintable(rules0.join(Q(","))));
    QVERIFY(!rules0.contains(Q("clipping")));
    QVERIFY(c0.value(Q("speechMargin")).toObject().value(Q("checked")).toBool());

    // duck: a dry run changes nothing; the speech ranges come from the energy gate (no index in this engine)
    const QJsonObject dry = ok(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}});
    QVERIFY(!dry.value(Q("applied")).toBool());
    QCOMPARE(dry.value(Q("rangeSource")).toString(), Q("energyGate"));
    QCOMPARE(dry.value(Q("ranges")).toArray().size(), 1);
    QVERIFY(dry.value(Q("depthComputed")).toBool());
    const double depth = dry.value(Q("depthDb")).toDouble();
    QVERIFY2(depth > 6 && depth < 12, qPrintable(QString::number(depth)));
    const double before = dry.value(Q("before")).toObject().value(Q("marginDb")).toDouble(), after = dry.value(Q("after")).toObject().value(Q("marginDb")).toDouble();
    QVERIFY2(std::fabs(after - 9.0) < 1.0 && after > before + 5, qPrintable(QStringLiteral("margin %1 -> %2 dB").arg(before).arg(after)));
    QVERIFY(dry.value(Q("keyframes")).toArray().size() >= 2);
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    // explicit ranges work too
    const QJsonObject ex = ok(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}, {Q("ranges"), QJsonArray{QJsonObject{{Q("startFrame"), 5}, {Q("endFrame"), 20}}}}, {Q("depthDb"), 12.0}});
    QCOMPARE(ex.value(Q("rangeSource")).toString(), Q("explicit"));
    QVERIFY(!ex.value(Q("depthComputed")).toBool());
    QCOMPARE(raw(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), videoTrack}}).error.code, Q("invalid_params"));

    // apply: one undo step, the margin measured on the real mix is the one that was predicted
    const QJsonObject dk = ok(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}, {Q("apply"), true}, {Q("origin"), Q("agent:audio")}});
    QVERIFY(dk.value(Q("applied")).toBool());
    const QJsonObject strip = ok(Q("mixer.get")).value(Q("mixer")).toObject().value(Q("strips")).toArray().at(0).toObject();
    QCOMPARE(strip.value(Q("id")).toString(), audioTrack);
    QVERIFY(!strip.value(Q("automation")).toObject().value(Q("volume")).toArray().isEmpty());
    QCOMPARE(raw(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}, {Q("apply"), true}}).error.code, Q("invalid_params")); // would overwrite automation
    const QJsonObject again = ok(Q("audio.duck"), {{Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}, {Q("replaceExisting"), true}});
    QVERIFY2(std::fabs(again.value(Q("before")).toObject().value(Q("marginDb")).toDouble() - after) < 0.3, qPrintable(QString::number(again.value(Q("before")).toObject().value(Q("marginDb")).toDouble())));
    ok(Q("history.undo"));
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    ok(Q("history.redo"));

    // normalize: dry run proposes, apply writes the master fader once and the re-measure confirms it
    const QJsonObject np = ok(Q("audio.normalize"));
    QVERIFY(!np.value(Q("applied")).toBool());
    const double gain = np.value(Q("proposal")).toObject().value(Q("gainDb")).toDouble();
    QVERIFY2(gain > 1 && gain < 15, qPrintable(QString::number(gain)));
    const double lufs0 = np.value(Q("before")).toObject().value(Q("integratedLufs")).toDouble();
    const QJsonObject na = ok(Q("audio.normalize"), {{Q("apply"), true}});
    QVERIFY(na.value(Q("applied")).toBool());
    QVERIFY(na.value(Q("verified")).toBool());
    const double lufs1 = na.value(Q("after")).toObject().value(Q("integratedLufs")).toDouble();
    QVERIFY2(std::fabs(lufs1 - (lufs0 + gain)) < 0.3, qPrintable(QStringLiteral("%1 + %2 -> %3").arg(lufs0).arg(gain).arg(lufs1)));
    QVERIFY2(std::fabs(lufs1 + 14.0) < 0.5 || na.value(Q("proposal")).toObject().value(Q("limitedByTruePeak")).toBool(), qPrintable(QString::number(lufs1)));
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).toObject().value(Q("master")).toObject().value(Q("volume")).toDouble() > 1.0);
    // a second pass has nothing left to do
    QVERIFY(std::fabs(ok(Q("audio.normalize")).value(Q("proposal")).toObject().value(Q("gainDb")).toDouble()) < 0.3);
    // an item is normalized on its volume
    const QJsonObject ni = ok(Q("audio.normalize"), {{Q("scope"), musicItem}, {Q("target"), -40.0}, {Q("apply"), true}});
    QVERIFY(ni.value(Q("applied")).toBool());
    QVERIFY(ok(Q("item.get"), {{Q("itemId"), musicItem}}).value(Q("volume")).toDouble() < 1.0);
    ok(Q("history.undo"));
    QCOMPARE(raw(Q("audio.normalize"), {{Q("scope"), firstAsset(Q("audio"))}}).error.code, Q("invalid_params"));

    // fix the fade, then check again: loudness and margin are fine, nothing else is wrong
    ok(Q("item.audio.set"), {{Q("itemId"), musicItem}, {Q("fadeOutFrames"), 6}});
    const QJsonObject c1 = ok(Q("audio.check"), {{Q("tolerance"), 1.5}});
    QVERIFY2(c1.value(Q("pass")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(c1).toJson())));
    // a spec the mix cannot meet is reported with its numbers
    const QJsonObject c2 = ok(Q("audio.check"), {{Q("target"), -30.0}, {Q("tolerance"), 0.5}, {Q("truePeakCeiling"), -20.0}});
    QStringList rules2;
    for (const QJsonValue v : c2.value(Q("violations")).toArray()) rules2 << v.toObject().value(Q("rule")).toString();
    QVERIFY(rules2.contains(Q("loudness")) && rules2.contains(Q("true_peak")));
    QVERIFY(c2.value(Q("violations")).toArray().at(0).toObject().contains(Q("measured")));
    qInfo().noquote() << QStringLiteral("audio: master %1 LUFS -> %2 LUFS after normalize (gain %3 dB); speech margin %4 -> %5 dB after duck (depth %6 dB)")
                             .arg(lufs0, 0, 'f', 2).arg(lufs1, 0, 'f', 2).arg(gain, 0, 'f', 2).arg(before, 0, 'f', 2).arg(after, 0, 'f', 2).arg(depth, 0, 'f', 2);

    // music.structure on a 3 s sine: nothing to find, and it says so instead of inventing a tempo
    const QJsonObject ms = ok(Q("music.structure"), {{Q("assetId"), firstAsset(Q("audio"))}});
    QVERIFY(!ms.value(Q("valid")).toBool());
    QVERIFY(ms.value(Q("note")).toString().size() > 5);
    QCOMPARE(raw(Q("music.structure"), {}).error.code, Q("invalid_params"));
    QVERIFY(e_->describe(Q("audio")).size() >= 5);
  }


  // ---- measured editing ----
  QString assetNamed(const QString& file) {
    for (const QJsonValue& a : ok(Q("assets.list")).value(Q("assets")).toArray())
      if (a.toObject().value(Q("path")).toString().endsWith(file)) return a.toObject().value(Q("id")).toString();
    return {};
  }
  QString addClip(const QString& asset, int start, int dur, int srcIn) {
    return ok(Q("clip.add"), {{Q("assetId"), asset}, {Q("startFrame"), start}, {Q("durationFrames"), dur}, {Q("sourceInFrame"), srcIn}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
  }
  static QStringList rulesOf(const QJsonObject& check) {
    QStringList r;
    for (const QJsonValue& v : check.value(Q("violations")).toArray()) r << v.toObject().value(Q("rule")).toString();
    return r;
  }

  void measuredEditing() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{story_, talk_, beats_}}});
    const QString story = assetNamed(Q("story.mp4")), beats = assetNamed(Q("beats.wav"));
    QVERIFY(!story.isEmpty() && !beats.isEmpty());
    const QString musicItem = ok(Q("audio.add"), {{Q("assetId"), beats}, {Q("startFrame"), 0}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();

    // the beat grid as music.structure measures it: 100 BPM = 15 frames at 25 fps
    const QJsonObject ms = ok(Q("music.structure"), {{Q("itemId"), musicItem}});
    QVERIFY2(ms.value(Q("valid")).toBool(), qPrintable(ms.value(Q("note")).toString()));
    QVERIFY2(std::fabs(ms.value(Q("bpm")).toDouble() - 100.0) < 2.0, qPrintable(QString::number(ms.value(Q("bpm")).toDouble())));
    const QJsonArray bf = ms.value(Q("beatFrames")).toArray();
    QVERIFY(bf.size() > 20);
    const auto beat = [&](int k) { return bf.at(k).toInt(); };

    // six shots whose five cuts sit 4, 3 late, 5 early, 1 late and 6 late of a beat: cut k = beat(2k+2) + off[k]
    const int off[5] = {4, 3, -5, 1, 6};
    QVector<int> bounds{0};
    for (int k = 0; k < 5; ++k) bounds << beat(2 * k + 2) + off[k];
    bounds << bounds.last() + 30;
    QStringList shots;
    for (int i = 0; i + 1 < bounds.size(); ++i) shots << addClip(story, bounds[i], bounds[i + 1] - bounds[i], 40);
    const QString storyTrack = ok(Q("item.get"), {{Q("itemId"), shots[0]}}).value(Q("trackId")).toString();

    // stats: shots, ASL, cuts, hook, and the beat alignment against the music
    const QJsonObject st = ok(Q("timeline.stats"));
    QCOMPARE(st.value(Q("shotCount")).toInt(), 6);
    QCOMPARE(st.value(Q("cutCount")).toInt(), 5);
    QCOMPARE(st.value(Q("trackId")).toString(), storyTrack);
    const double expectedAsl = bounds.last() / 25.0 / 6.0;
    QVERIFY2(std::fabs(st.value(Q("aslSec")).toDouble() - expectedAsl) < 0.01, qPrintable(QString::number(st.value(Q("aslSec")).toDouble())));
    QVERIFY(std::fabs(st.value(Q("firstCutSec")).toDouble() - bounds[1] / 25.0) < 0.01);
    QCOMPARE(st.value(Q("shortest")).toArray().size(), 5);
    QVERIFY(st.value(Q("cutRhythm")).toObject().value(Q("cutsPerWindow")).toArray().size() >= 1);
    const QJsonObject al0 = st.value(Q("beatAlignment")).toObject();
    QVERIFY2(al0.value(Q("available")).toBool(), qPrintable(al0.value(Q("reason")).toString()));
    const double pct0 = al0.value(Q("beat")).toObject().value(Q("withinPct")).toDouble();
    QCOMPARE(pct0, 20.0); // only the cut 1 frame off the beat is within +-2 frames
    QVERIFY(raw(Q("timeline.stats"), {{Q("trackId"), Q("trk_nope")}}).error.code == Q("not_found"));
    QCOMPARE(ok(Q("timeline.stats"), {{Q("startFrame"), bounds[1]}, {Q("endFrame"), bounds[3]}}).value(Q("cutCount")).toInt(), 1);

    // check before: five cuts off the beat; the rest of the edit is clean
    const QJsonObject c0 = ok(Q("timeline.check"), {{Q("maxOffBeatMs"), 30.0}});
    QVERIFY(!c0.value(Q("pass")).toBool());
    QCOMPARE(rulesOf(c0).count(Q("off_beat")), 5);
    QCOMPARE(rulesOf(c0).size(), 5);
    QVERIFY(c0.value(Q("violations")).toArray().at(0).toObject().contains(Q("measured")));
    QVERIFY(ok(Q("timeline.check")).value(Q("pass")).toBool()); // without maxOffBeatMs there is no off-beat rule

    // snap: a dry run proposes and changes nothing
    const QJsonObject hist0 = ok(Q("history.list"));
    const QJsonObject dry = ok(Q("edit.snapToBeats"));
    QVERIFY(!dry.value(Q("applied")).toBool());
    QCOMPARE(dry.value(Q("moved")).toInt(), 5);
    QCOMPARE(dry.value(Q("skipped")).toArray().size(), 0);
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt());
    const QJsonArray planned = dry.value(Q("cuts")).toArray();
    QCOMPARE(planned.size(), 5);
    for (int k = 0; k < 5; ++k) {
      const QJsonObject c = planned.at(k).toObject();
      QCOMPARE(c.value(Q("frame")).toInt(), bounds[k + 1]);
      QCOMPARE(c.value(Q("targetFrame")).toInt(), beat(2 * k + 2));
      QCOMPARE(c.value(Q("shiftFrames")).toInt(), -off[k]);
      QVERIFY(c.value(Q("method")).toString().contains(Q("item.trim")));
    }
    QCOMPARE(dry.value(Q("predicted")).toObject().value(Q("withinPct")).toDouble(), 100.0);
    // a tight reach leaves the far cuts alone and says why
    const QJsonObject nearOnly = ok(Q("edit.snapToBeats"), {{Q("maxShiftFrames"), 3}});
    QCOMPARE(nearOnly.value(Q("moved")).toInt(), 2); // the 3-frame and 1-frame cuts
    QCOMPARE(nearOnly.value(Q("skipped")).toArray().size(), 3);
    QCOMPARE(nearOnly.value(Q("skipped")).toArray().at(0).toObject().value(Q("reason")).toString(), Q("too_far"));
    // downbeats are every 4th beat: fewer targets
    const QJsonObject down = ok(Q("edit.snapToBeats"), {{Q("to"), Q("downbeat")}, {Q("maxShiftFrames"), 6}});
    QCOMPARE(down.value(Q("to")).toString(), Q("downbeat"));
    QVERIFY(down.value(Q("gridFrames")).toInt() < dry.value(Q("gridFrames")).toInt());
    QCOMPARE(raw(Q("edit.snapToBeats"), {{Q("itemIds"), QJsonArray{Q("itm_nope")}}}).error.code, Q("not_found"));

    // apply: one undo step, alignment measured again and better
    const QJsonObject ap = ok(Q("edit.snapToBeats"), {{Q("apply"), true}, {Q("origin"), Q("agent:editor")}});
    QVERIFY(ap.value(Q("applied")).toBool());
    QVERIFY(ap.value(Q("verified")).toBool());
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt() + 1);
    const double pct1 = ap.value(Q("after")).toObject().value(Q("withinPct")).toDouble();
    QCOMPARE(pct1, 100.0);
    QVERIFY(ap.value(Q("after")).toObject().value(Q("meanAbsOffsetMs")).toDouble() < 1e-6);
    const QJsonObject st1 = ok(Q("timeline.stats"));
    QCOMPARE(st1.value(Q("beatAlignment")).toObject().value(Q("beat")).toObject().value(Q("withinPct")).toDouble(), 100.0);
    QCOMPARE(st1.value(Q("shotCount")).toInt(), 6);
    QCOMPARE(st1.value(Q("durationSec")).toDouble(), st.value(Q("durationSec")).toDouble()); // trims, not ripples: the length is unchanged
    QVERIFY(ok(Q("timeline.check"), {{Q("maxOffBeatMs"), 30.0}}).value(Q("pass")).toBool());
    // the trims moved the source in points with the cuts: the second shot now starts 4 frames earlier in the source
    QCOMPARE(ok(Q("item.get"), {{Q("itemId"), shots[1]}}).value(Q("sourceInFrame")).toInt(), 40 - off[0]);
    // already on the grid: nothing to write
    const QJsonObject again = ok(Q("edit.snapToBeats"), {{Q("apply"), true}});
    QVERIFY(!again.value(Q("applied")).toBool());
    QCOMPARE(again.value(Q("alreadyOnGrid")).toInt(), 5);
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(ok(Q("timeline.stats")).value(Q("beatAlignment")).toObject().value(Q("beat")).toObject().value(Q("withinPct")).toDouble(), 20.0);

    // handles: a cut that needs media before the in point of a shot read from source frame 0
    ok(Q("item.slip"), {{Q("itemId"), shots[1]}, {Q("sourceInFrame"), 0}});
    const QJsonObject nh = ok(Q("edit.snapToBeats"), {{Q("itemIds"), QJsonArray{shots[1]}}});
    QCOMPARE(nh.value(Q("moved")).toInt(), 0);
    QCOMPARE(nh.value(Q("skipped")).toArray().at(0).toObject().value(Q("reason")).toString(), Q("exceeds_handles"));

    // gaps, overlaps, flash frames, duration and aspect are reported with their numbers
    ok(Q("item.move"), {{Q("itemId"), shots[3]}, {Q("startFrame"), bounds[3] + 4}}); // 4 frames of nothing before it and 4 overlapping the next
    ok(Q("item.trim"), {{Q("itemId"), shots[4]}, {Q("edge"), Q("out")}, {Q("frame"), bounds[4] + 2}}); // a 2 frame shot
    const QJsonObject bad = ok(Q("timeline.check"), {{Q("targetDurationSec"), 30.0}, {Q("aspect"), Q("9:16")}});
    const QStringList badRules = rulesOf(bad);
    for (const char* r : {"gap", "flash_frame", "duration", "aspect_canvas"}) QVERIFY2(badRules.contains(Q(r)), qPrintable(Q(r) + Q(" in ") + badRules.join(Q(","))));
    QVERIFY(!badRules.contains(Q("word_clip")));
    QCOMPARE(raw(Q("timeline.check"), {{Q("aspect"), Q("wide")}}).error.code, Q("invalid_params"));
    qInfo().noquote() << QStringLiteral("editing: %1 cuts, ASL %2 s; on-beat share %3 percent -> %4 percent after snapToBeats (%5 moved), mean offset %6 ms -> %7 ms")
                             .arg(st.value(Q("cutCount")).toInt()).arg(st.value(Q("aslSec")).toDouble(), 0, 'f', 2).arg(pct0).arg(pct1).arg(ap.value(Q("moved")).toInt())
                             .arg(al0.value(Q("beat")).toObject().value(Q("meanOffsetMs")).toDouble(), 0, 'f', 1).arg(ap.value(Q("after")).toObject().value(Q("meanOffsetMs")).toDouble(), 0, 'f', 1);
  }

  void musicFit() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{beats_}}});
    const QString beats = assetNamed(Q("beats.wav"));
    QVERIFY(!beats.isEmpty());
    const QString musicItem = ok(Q("audio.add"), {{Q("assetId"), beats}, {Q("startFrame"), 0}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();

    const QJsonObject origItem = ok(Q("item.get"), {{Q("itemId"), musicItem}});
    const QString trackId = origItem.value(Q("trackId")).toString();
    const int origDuration = origItem.value(Q("durationFrames")).toInt();
    const int origSrcIn = origItem.value(Q("sourceInFrame")).toInt();
    const int targetFrames = static_cast<int>(std::round(origDuration * 0.6));

    const auto getTrackItems = [&]() {
      QJsonArray list;
      for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("mode"), Q("detail")}}).value(Q("tracks")).toArray()) {
        if (t.toObject().value(Q("id")).toString() == trackId) {
          list = t.toObject().value(Q("items")).toArray();
          break;
        }
      }
      return list;
    };

    // 1. Dry run: apply=false
    const QJsonObject dry = ok(Q("music.fit"), {{Q("itemId"), musicItem}, {Q("targetFrames"), targetFrames}, {Q("apply"), false}});
    QVERIFY(!dry.contains(Q("__failed")));
    QVERIFY(!dry.value(Q("applied")).toBool());
    const QJsonArray segs = dry.value(Q("segments")).toArray();
    QVERIFY(!segs.isEmpty());

    int segSum = 0;
    for (const QJsonValue& v : segs) {
      const QJsonObject s = v.toObject();
      segSum += s.value(Q("endFrame")).toInt() - s.value(Q("startFrame")).toInt();
    }
    QVERIFY2(std::abs(segSum - targetFrames) <= 1, qPrintable(QStringLiteral("segSum %1 vs target %2").arg(segSum).arg(targetFrames)));
    QCOMPARE(getTrackItems().size(), 1);

    // 2. Apply: apply=true
    const QJsonObject applied = ok(Q("music.fit"), {{Q("itemId"), musicItem}, {Q("targetFrames"), targetFrames}, {Q("apply"), true}});
    QVERIFY(!applied.contains(Q("__failed")));
    QVERIFY(applied.value(Q("applied")).toBool());

    const QJsonArray trackItems = getTrackItems();
    QVERIFY(!trackItems.isEmpty());
    int firstStart = trackItems.at(0).toObject().value(Q("startFrame")).toInt();
    int lastEnd = trackItems.at(0).toObject().value(Q("startFrame")).toInt() + trackItems.at(0).toObject().value(Q("durationFrames")).toInt();
    for (const QJsonValue& itv : trackItems) {
      int sf = itv.toObject().value(Q("startFrame")).toInt();
      int ef = sf + itv.toObject().value(Q("durationFrames")).toInt();
      firstStart = std::min(firstStart, sf);
      lastEnd = std::max(lastEnd, ef);
    }
    int totalLen = lastEnd - firstStart;
    QVERIFY2(std::abs(totalLen - targetFrames) <= 1, qPrintable(QStringLiteral("totalLen %1 vs target %2").arg(totalLen).arg(targetFrames)));

    // 3. Undo once: track is back to exactly 1 item with original source in/out
    ok(Q("history.undo"));
    const QJsonArray restored = getTrackItems();
    QCOMPARE(restored.size(), 1);
    const QJsonObject restoredItem = ok(Q("item.get"), {{Q("itemId"), musicItem}});
    QCOMPARE(restoredItem.value(Q("durationFrames")).toInt(), origDuration);
    QCOMPARE(restoredItem.value(Q("sourceInFrame")).toInt(), origSrcIn);
  }

  void speechSafeCuts() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{talk_}}});
    const QString talk = assetNamed(Q("talk.mp4"));
    // the tone speaks in [1,2) s of every 2 s: frames 25..50 of the source. A holds source 0..40, B holds 40..80
    const QString a = addClip(talk, 0, 40, 0), b = addClip(talk, 40, 40, 40);
    const QJsonObject mid = ok(Q("edit.safeCutPoint"), {{Q("itemId"), a}, {Q("frame"), 38}});
    QCOMPARE(mid.value(Q("speechSource")).toString(), Q("energyGate"));
    QVERIFY(mid.value(Q("insideWord")).toBool());
    QVERIFY(mid.value(Q("safe")).toBool());
    QVERIFY2(std::abs(mid.value(Q("safeFrame")).toInt() - 40) <= 2, qPrintable(QString::number(mid.value(Q("safeFrame")).toInt())));
    const QJsonObject head = ok(Q("edit.safeCutPoint"), {{Q("itemId"), a}, {Q("frame"), 30}});
    QVERIFY(std::abs(head.value(Q("safeFrame")).toInt() - 25) <= 2);
    QVERIFY(head.value(Q("shiftFrames")).toInt() < 0);
    const QJsonObject far = ok(Q("edit.safeCutPoint"), {{Q("itemId"), a}, {Q("frame"), 32}, {Q("maxShiftFrames"), 3}});
    QVERIFY(far.value(Q("insideWord")).toBool());
    QVERIFY(!far.value(Q("safe")).toBool());
    QVERIFY(far.value(Q("safeFrame")).isNull());
    QVERIFY(far.contains(Q("alternatives")));
    const QJsonObject gap = ok(Q("edit.safeCutPoint"), {{Q("itemId"), b}, {Q("frame"), 60}}); // 60 is in the silence 50..75
    QVERIFY(!gap.value(Q("insideWord")).toBool());
    QCOMPARE(gap.value(Q("safeFrame")).toInt(), 60);
    QCOMPARE(raw(Q("edit.safeCutPoint"), {{Q("itemId"), Q("itm_nope")}, {Q("frame"), 3}}).error.code, Q("not_found"));

    // the cut between A and B (frame 40) runs through one word: timeline.check says so
    const QJsonObject ck = ok(Q("timeline.check"));
    QVERIFY(!ck.value(Q("pass")).toBool());
    QStringList words;
    for (const QJsonValue& v : ck.value(Q("violations")).toArray())
      if (v.toObject().value(Q("rule")).toString() == Q("word_clip")) words << QString::number(v.toObject().value(Q("frame")).toInt());
    QCOMPARE(words, QStringList{Q("40")});
    // moving the cut out of the word fixes it: both sides of the cut at 52, in the silence after the word
    ok(Q("item.trim"), {{Q("itemId"), b}, {Q("edge"), Q("in")}, {Q("frame"), 52}});
    ok(Q("item.trim"), {{Q("itemId"), a}, {Q("edge"), Q("out")}, {Q("frame"), 52}});
    QVERIFY(ok(Q("timeline.check")).value(Q("pass")).toBool());

    // snapToBeats refuses a target that lands inside a word, and allowWordClip overrides
    const QJsonObject word = ok(Q("edit.snapToBeats"), {{Q("beatFrames"), QJsonArray{44, 59}}, {Q("maxShiftFrames"), 10}});
    QCOMPARE(word.value(Q("moved")).toInt(), 1); // the cut at 52 goes to 59 (in the silence) not 44 (in the word): 7 away vs 8
    QCOMPARE(word.value(Q("cuts")).toArray().at(0).toObject().value(Q("targetFrame")).toInt(), 59);
    const QJsonObject refused = ok(Q("edit.snapToBeats"), {{Q("beatFrames"), QJsonArray{47}}, {Q("maxShiftFrames"), 10}});
    QCOMPARE(refused.value(Q("moved")).toInt(), 0);
    const QJsonObject rs = refused.value(Q("skipped")).toArray().at(0).toObject();
    QCOMPARE(rs.value(Q("reason")).toString(), Q("clips_word"));
    QVERIFY(rs.contains(Q("suggestedFrame")));
    const QJsonObject forced = ok(Q("edit.snapToBeats"), {{Q("beatFrames"), QJsonArray{47}}, {Q("maxShiftFrames"), 10}, {Q("allowWordClip"), true}});
    QCOMPARE(forced.value(Q("moved")).toInt(), 1);
  }

  void snapMovesLinkedAudio() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{story_, beats_}}});
    const QString story = assetNamed(Q("story.mp4")), beats = assetNamed(Q("beats.wav"));
    const QString a = addClip(story, 0, 40, 40), b = addClip(story, 40, 40, 80);
    const auto addAudio = [&](int start, int dur, int srcIn) {
      return ok(Q("audio.add"), {{Q("assetId"), beats}, {Q("startFrame"), start}, {Q("durationFrames"), dur}, {Q("sourceInFrame"), srcIn}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    };
    const QString aa = addAudio(0, 40, 40), ab = addAudio(40, 40, 80);
    const int total0 = ok(Q("history.list")).value(Q("total")).toInt();
    const QJsonObject ap = ok(Q("edit.snapToBeats"), {{Q("beatFrames"), QJsonArray{44}}, {Q("allowWordClip"), true}, {Q("apply"), true}, {Q("trackId"), ok(Q("item.get"), {{Q("itemId"), a}}).value(Q("trackId")).toString()}});
    QVERIFY(ap.value(Q("applied")).toBool());
    QCOMPARE(ap.value(Q("linkedAudioMoved")).toArray().size(), 2);
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), total0 + 1); // picture and sound in one undo step
    const auto get = [&](const QString& id) { return ok(Q("item.get"), {{Q("itemId"), id}}); };
    QCOMPARE(get(a).value(Q("durationFrames")).toInt(), 44);
    QCOMPARE(get(aa).value(Q("durationFrames")).toInt(), 44);
    QCOMPARE(get(b).value(Q("startFrame")).toInt(), 44);
    QCOMPARE(get(ab).value(Q("startFrame")).toInt(), 44);
    QCOMPARE(get(ab).value(Q("durationFrames")).toInt(), 36);
    QCOMPARE(get(ab).value(Q("sourceInFrame")).toInt(), 84);
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QCOMPARE(get(aa).value(Q("durationFrames")).toInt(), 40);
    QCOMPARE(get(ab).value(Q("startFrame")).toInt(), 40);
  }

  void aspectMismatchUsesTransform() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{vert_}}});
    const QString vert = assetNamed(Q("vert.mp4"));
    const QString v = addClip(vert, 0, 25, 0);
    // default placement: a vertical clip pillarboxed on the 16:9 canvas is a note, not a violation
    QJsonObject ck = ok(Q("timeline.check"));
    QVERIFY(!rulesOf(ck).contains(Q("aspect_mismatch")));
    QCOMPARE(ck.value(Q("info")).toArray().size(), 1);
    QCOMPARE(ck.value(Q("info")).toArray().at(0).toObject().value(Q("kind")).toString(), Q("pillarbox"));
    // scaled to fill the canvas: nothing to report
    ok(Q("item.update"), {{Q("itemId"), v}, {Q("patch"), QJsonObject{{Q("transform"), QJsonObject{{Q("scale"), 3.3}}}}}});
    ck = ok(Q("timeline.check"));
    QVERIFY(!rulesOf(ck).contains(Q("aspect_mismatch")));
    QCOMPARE(ck.value(Q("info")).toArray().size(), 0);
    // shrunk so it neither fills nor is letter / pillarboxed: flagged
    ok(Q("item.update"), {{Q("itemId"), v}, {Q("patch"), QJsonObject{{Q("transform"), QJsonObject{{Q("scale"), 0.5}}}}}});
    QVERIFY(rulesOf(ok(Q("timeline.check"))).contains(Q("aspect_mismatch")));
  }


  // ---- system profile ----
  void systemProfileCommands() {
    const QJsonObject p = ok(Q("system.profile"));
    QVERIFY(p.value(Q("profile")).toObject().value(Q("cpu")).toObject().value(Q("logicalCores")).toInt() >= 1);
    const QJsonObject smoke = p.value(Q("smoke")).toObject();
    QVERIFY(smoke.value(Q("ran")).toBool());
    QCOMPARE(smoke.value(Q("encoders")).toArray().size(), 4);
    QVERIFY(!smoke.value(Q("fromCache")).toBool());
    const QJsonObject d = p.value(Q("decisions")).toObject();
    for (const char* k : {"previewScale", "hwDecode", "exportEncoder", "mlDevice", "mlThreads", "backgroundPriority", "cacheMb", "reasons"}) QVERIFY2(d.contains(Q(k)), k);
    qInfo().noquote() << QStringLiteral("system.profile smoke: %1 ms, compositor %2; decisions %3").arg(smoke.value(Q("totalMs")).toDouble(), 0, 'f', 0)
                             .arg(QString::fromUtf8(QJsonDocument(smoke.value(Q("compositor")).toObject()).toJson(QJsonDocument::Compact)), QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)));
    QVERIFY(ok(Q("system.profile")).value(Q("smoke")).toObject().value(Q("fromCache")).toBool()); // cached the second time
    const QJsonObject set = ok(Q("system.overrides.set"), {{Q("key"), Q("cacheMb")}, {Q("value"), 321}});
    QCOMPARE(set.value(Q("decisions")).toObject().value(Q("cacheMb")).toInt(), 321);
    QCOMPARE(ok(Q("system.profile"), {{Q("smoke"), false}}).value(Q("decisions")).toObject().value(Q("cacheMb")).toInt(), 321);
    ok(Q("system.overrides.set"), {{Q("key"), Q("cacheMb")}, {Q("value"), QJsonValue(QJsonValue::Null)}});
    QVERIFY(ok(Q("system.profile"), {{Q("smoke"), false}}).value(Q("decisions")).toObject().value(Q("cacheMb")).toInt() != 321);
    QCOMPARE(raw(Q("system.overrides.set"), {{Q("key"), Q("mlDevice")}, {Q("value"), Q("tpu")}}).error.code, Q("invalid_params"));
    const QJsonObject fit = ok(Q("llm.fitAdvice"), {{Q("sizeBytes"), 4.0 * 1024 * 1024 * 1024}, {Q("layers"), 32}, {Q("vramBudgetBytes"), 12.0 * 1024 * 1024 * 1024}});
    QVERIFY(fit.value(Q("fitsFully")).toBool());
    QCOMPARE(fit.value(Q("ngl")).toInt(), 32);
    QCOMPARE(raw(Q("llm.fitAdvice"), {}).error.code, Q("invalid_params"));
  }

  // ---- style library ----
  void styleLibraryCommands() {
    const QJsonObject all = ok(Q("style.list"));
    QCOMPARE(all.value(Q("looks")).toObject().value(Q("items")).toArray().size(), 8);
    QCOMPARE(all.value(Q("pacing")).toObject().value(Q("items")).toArray().size(), 5);
    QCOMPARE(all.value(Q("mix")).toObject().value(Q("items")).toArray().size(), 4);
    const QJsonObject one = ok(Q("style.list"), {{Q("kind"), Q("look")}, {Q("name"), Q("teal-orange")}});
    QCOMPARE(one.value(Q("items")).toArray().size(), 1);
    const QJsonObject look = one.value(Q("items")).toArray().at(0).toObject();
    QVERIFY(look.contains(Q("params")) && look.contains(Q("guards")) && look.contains(Q("description")));
    QVERIFY(one.value(Q("schema")).toObject().contains(Q("properties")));
    QCOMPARE(raw(Q("style.list"), {{Q("kind"), Q("look")}, {Q("name"), Q("sepia")}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("style.list"), {{Q("kind"), Q("grade")}}).error.code, Q("invalid_params"));
    const QJsonObject sp = ok(Q("style.spec.schema"));
    QVERIFY(sp.value(Q("schema")).toObject().value(Q("properties")).toObject().contains(Q("intensity")));
    QVERIFY(sp.contains(Q("example")));
    // no session needed for reading the library
    Engine bare;
    QVERIFY(bare.call(Q("style.list")).ok);
    for (const char* n : {"style.list", "style.spec.schema", "look.apply", "pacing.check", "mix.applyProfile"}) QVERIFY2(e_->find(Q(n)), n);
  }

  // ---- C12: audio presets, style.suggest, LUT export ----
  void presetsSuggestAndLuts() {
    for (const char* n : {"audio.preset.list", "audio.preset.apply", "style.suggest", "look.exportLut", "look.exportLuts"}) QVERIFY2(e_->find(Q(n)), n);
    // list: filters; every item carries tags and a chain
    const QJsonObject all = ok(Q("audio.preset.list"));
    QCOMPARE(all.value(Q("items")).toArray().size(), 16);
    const QJsonObject voice = ok(Q("audio.preset.list"), {{Q("category"), Q("voice")}});
    QCOMPARE(voice.value(Q("items")).toArray().size(), 6);
    QVERIFY(ok(Q("audio.preset.list"), {{Q("tag"), Q("noisy")}}).value(Q("ids")).toArray().contains(Q("gate_noisy_room")));
    const QJsonObject first = all.value(Q("items")).toArray().at(0).toObject();
    QVERIFY(first.contains(Q("whenToUse")) && first.contains(Q("avoidWhen")) && !first.value(Q("chain")).toArray().isEmpty());
    // style.list now carries the tags
    QVERIFY(!ok(Q("style.list"), {{Q("kind"), Q("mix")}, {Q("name"), Q("podcast")}}).value(Q("items")).toArray().at(0).toObject().value(Q("whenToUse")).toArray().isEmpty());

    // suggest: deterministic, scored, ties by id; every kind answers
    const QJsonObject sug = ok(Q("style.suggest"), {{Q("kind"), Q("audio")}, {Q("tags"), QJsonArray{Q("music-bed"), Q("voice-over")}}});
    QCOMPARE(sug.value(Q("ranked")).toArray().at(0).toObject().value(Q("id")).toString(), Q("music_bed_under_voice"));
    QCOMPARE(sug.value(Q("ranked")).toArray().at(0).toObject().value(Q("score")).toInt(), 2);
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("audio")}, {Q("tags"), QJsonArray{Q("music-bed"), Q("voice-over")}}}), sug);
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("look")}, {Q("tags"), QJsonArray{Q("interview")}}}).value(Q("ranked")).toArray().at(0).toObject().value(Q("id")).toString(), Q("neutral-clean"));
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("pacing")}, {Q("tags"), QJsonArray{Q("social"), Q("hook")}}}).value(Q("ranked")).toArray().at(0).toObject().value(Q("id")).toString(), Q("social-short"));
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("mix")}, {Q("tags"), QJsonArray{Q("podcast")}}}).value(Q("ranked")).toArray().at(0).toObject().value(Q("id")).toString(), Q("podcast"));
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("effect")}, {Q("tags"), QJsonArray{Q("blur")}}, {Q("limit"), 3}}).value(Q("ranked")).toArray().size(), 3);
    QCOMPARE(ok(Q("style.suggest"), {{Q("kind"), Q("look")}, {Q("tags"), QJsonArray{Q("interview")}}, {Q("limit"), 0}}).value(Q("ranked")).toArray().size(), 8);
    QCOMPARE(raw(Q("style.suggest"), {{Q("kind"), Q("look")}, {Q("tags"), QJsonArray{}}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("style.suggest"), {{Q("kind"), Q("filter")}, {Q("tags"), QJsonArray{Q("x")}}}).error.code, Q("invalid_params"));

    // apply: one undo step, measured before / after
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_}}});
    ok(Q("clip.add"), {{Q("assetId"), firstAsset(Q("video"))}});
    ok(Q("audio.add"), {{Q("assetId"), firstAsset(Q("audio"))}});
    QString audioTrack, musicItem;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("mode"), Q("summary")}}).value(Q("tracks")).toArray()) {
      const QJsonObject to = t.toObject();
      if (to.value(Q("items")).toArray().isEmpty() || to.value(Q("kind")).toString() != Q("audio")) continue;
      audioTrack = to.value(Q("id")).toString();
      musicItem = to.value(Q("items")).toArray().at(0).toObject().value(Q("id")).toString();
    }
    QVERIFY(!audioTrack.isEmpty() && !musicItem.isEmpty());
    const QJsonObject hist0 = ok(Q("history.list"));
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    const QJsonObject ap = ok(Q("audio.preset.apply"), {{Q("trackId"), audioTrack}, {Q("presetId"), Q("dialogue_clean")}, {Q("origin"), Q("agent:audio")}});
    QVERIFY(ap.value(Q("applied")).toBool());
    QCOMPARE(ap.value(Q("effectIds")).toArray().size(), 3);
    QVERIFY(std::isfinite(ap.value(Q("before")).toObject().value(Q("integratedLufs")).toDouble(1000)) && ap.value(Q("before")).toObject().value(Q("integratedLufs")).toDouble(1000) < 0);
    QVERIFY(ap.value(Q("after")).toObject().contains(Q("truePeakDbtp")));
    const auto insertsOf = [&] {
      QJsonArray strips = ok(Q("mixer.get")).value(Q("mixer")).toObject().value(Q("strips")).toArray();
      return strips.isEmpty() ? QJsonArray{} : strips.at(0).toObject().value(Q("inserts")).toArray();
    };
    QJsonArray ins = insertsOf();
    QCOMPARE(ins.size(), 3);
    QCOMPARE(ins.at(0).toObject().value(Q("type")).toString(), Q("eq"));
    QCOMPARE(ins.at(1).toObject().value(Q("type")).toString(), Q("compressor"));
    QCOMPARE(ins.at(2).toObject().value(Q("type")).toString(), Q("deesser"));
    QCOMPARE(ins.at(0).toObject().value(Q("id")).toString(), ap.value(Q("effectIds")).toArray().at(0).toString());
    QCOMPARE(ins.at(0).toObject().value(Q("params")).toObject().value(Q("b0.type")).toString(), Q("highpass"));
    // stacks by default; replace drops what was there; an itemId resolves to its track
    ok(Q("audio.preset.apply"), {{Q("itemId"), musicItem}, {Q("presetId"), Q("rumble_cut")}, {Q("measure"), false}});
    QCOMPARE(insertsOf().size(), 4);
    const QJsonObject rp = ok(Q("audio.preset.apply"), {{Q("trackId"), audioTrack}, {Q("presetId"), Q("master_streaming_-14")}, {Q("replace"), true}, {Q("measure"), false}});
    QVERIFY(!rp.contains(Q("before")));
    QCOMPARE(insertsOf().size(), 1);
    QCOMPARE(insertsOf().at(0).toObject().value(Q("type")).toString(), Q("limiter"));
    // each apply is one undo step
    ok(Q("history.undo"));
    QCOMPARE(insertsOf().size(), 4);
    ok(Q("history.undo"));
    QCOMPARE(insertsOf().size(), 3);
    ok(Q("history.undo"));
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    QCOMPARE(ok(Q("history.list")).value(Q("position")).toInt(), hist0.value(Q("position")).toInt()); // back to before the first apply
    ok(Q("history.redo"));
    QCOMPARE(insertsOf().size(), 3);
    // master works too; errors
    QVERIFY(ok(Q("audio.preset.apply"), {{Q("trackId"), Q("master")}, {Q("presetId"), Q("master_broadcast_-23")}, {Q("measure"), false}}).value(Q("applied")).toBool());
    QCOMPARE(ok(Q("mixer.get")).value(Q("mixer")).toObject().value(Q("master")).toObject().value(Q("inserts")).toArray().size(), 2);
    QCOMPARE(raw(Q("audio.preset.apply"), {{Q("presetId"), Q("nope")}, {Q("trackId"), audioTrack}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("audio.preset.apply"), {{Q("presetId"), Q("lofi")}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("audio.preset.apply"), {{Q("presetId"), Q("lofi")}, {Q("trackId"), Q("trk_nope")}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("audio.preset.apply"), {{Q("presetId"), Q("lofi")}, {Q("itemId"), Q("itm_nope")}}).error.code, Q("not_found"));

    // LUT export: a file with the right header, an identity at intensity 0, all eight looks
    const QString lut = dir_.filePath(QStringLiteral("luts/warm.cube"));
    const QJsonObject ex = ok(Q("look.exportLut"), {{Q("lookId"), Q("warm-documentary")}, {Q("size"), 9}, {Q("path"), lut}});
    QCOMPARE(ex.value(Q("entries")).toInt(), 729);
    QCOMPARE(ex.value(Q("inputSpace")).toString(), Q("srgb"));
    QFile f(lut);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    f.close();
    QVERIFY(lines[0].startsWith(Q("TITLE \"")) && lines[0].contains(Q("warm-documentary")));
    QCOMPARE(lines[1], Q("LUT_3D_SIZE 9"));
    QCOMPARE(lines[2], Q("DOMAIN_MIN 0.0 0.0 0.0"));
    QCOMPARE(lines[3], Q("DOMAIN_MAX 1.0 1.0 1.0"));
    QCOMPARE(lines.size(), 4 + 729 + 1);
    QVERIFY(lines[4 + 364].split(QLatin1Char(' ')).at(0).toDouble() > 0.5); // mid grey is warmed: red above blue
    QVERIFY(lines[4 + 364].split(QLatin1Char(' ')).at(0).toDouble() > lines[4 + 364].split(QLatin1Char(' ')).at(2).toDouble());
    const QJsonObject idLut = ok(Q("look.exportLut"), {{Q("lookId"), Q("teal-orange")}, {Q("intensity"), 0.0}, {Q("size"), 5}, {Q("path"), dir_.filePath(QStringLiteral("luts/id.cube"))}});
    QVERIFY(idLut.value(Q("identity")).toBool());
    QCOMPARE(raw(Q("look.exportLut"), {{Q("lookId"), Q("sepia")}, {Q("path"), lut}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("look.exportLut"), {{Q("lookId"), Q("pastel")}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("look.exportLut"), {{Q("lookId"), Q("pastel")}, {Q("path"), lut}, {Q("size"), 1}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("look.exportLut"), {{Q("lookId"), Q("pastel")}, {Q("path"), lut}, {Q("inputSpace"), Q("acescg")}}).error.code, Q("invalid_params"));
    const QJsonObject lots = ok(Q("look.exportLuts"), {{Q("dir"), dir_.filePath(QStringLiteral("all"))}, {Q("size"), 5}});
    QCOMPARE(lots.value(Q("count")).toInt(), 8);
    for (const QJsonValue& v : lots.value(Q("files")).toArray()) QVERIFY(QFileInfo::exists(v.toObject().value(Q("path")).toString()));
    // no session needed
    QVERIFY(Engine().call(Q("audio.preset.list")).ok);
    QVERIFY(Engine().call(Q("style.suggest"), {{Q("kind"), Q("mix")}, {Q("tags"), QJsonArray{Q("social")}}}).ok);
  }

  static double castOf(const QJsonObject& metrics) { return metrics.value(Q("cast")).toObject().value(Q("temperature")).toDouble(); }

  void lookApplyOnAGreyClip() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{gray_}}});
    const QString item = addClip(assetNamed(Q("gray.mp4")), 0, 40, 0);
    const Result probe = raw(Q("colour.analyze"), {{Q("itemId"), item}, {Q("samples"), 2}});
    if (!probe.ok && probe.error.code == Q("unavailable") && probe.error.message.contains(Q("renderer"))) QSKIP("no D3D11 device for the offscreen renderer");
    QVERIFY2(probe.ok, qPrintable(probe.error.message));
    const double cast0 = castOf(probe.value.toObject());
    QVERIFY2(std::fabs(cast0) < 0.03, qPrintable(QString::number(cast0)));

    // dry run: the proposal and its prediction, nothing written
    const QJsonObject hist0 = ok(Q("history.list"));
    const QJsonObject dry = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}});
    QVERIFY(!dry.value(Q("applied")).toBool());
    QVERIFY2(dry.value(Q("ok")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(dry).toJson())));
    const QJsonObject it0 = dry.value(Q("items")).toArray().at(0).toObject();
    QCOMPARE(it0.value(Q("proposedPrimaries")).toObject().value(Q("temperature")).toDouble(), 0.14);
    QVERIFY(it0.value(Q("predicted")).toObject().value(Q("castTemperature")).toDouble() > it0.value(Q("before")).toObject().value(Q("castTemperature")).toDouble() + 0.08);
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), item}}).contains(Q("color")));
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt());

    // intensity scales the grade and the cast with it
    const QJsonObject halfDry = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("intensity"), 0.5}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}});
    const QJsonObject half = halfDry.value(Q("items")).toArray().at(0).toObject();
    QVERIFY(std::fabs(half.value(Q("proposedPrimaries")).toObject().value(Q("temperature")).toDouble() - 0.07) < 1e-9);
    const double pHalf = half.value(Q("predicted")).toObject().value(Q("castTemperature")).toDouble(), pFull = it0.value(Q("predicted")).toObject().value(Q("castTemperature")).toDouble();
    QVERIFY2(pHalf > cast0 + 0.03 && pHalf < pFull - 0.03, qPrintable(QStringLiteral("%1 < %2 < %3").arg(cast0).arg(pHalf).arg(pFull)));
    // the bounded offsets nudge it
    const QJsonObject nudged = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("warmth"), 0.1}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 1}}).value(Q("items")).toArray().at(0).toObject();
    QVERIFY(std::fabs(nudged.value(Q("proposedPrimaries")).toObject().value(Q("temperature")).toDouble() - 0.24) < 1e-9);
    QCOMPARE(raw(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("warmth"), 0.9}, {Q("itemIds"), QJsonArray{item}}}).error.code, Q("invalid_params"));

    // apply: one undo step, and the clip measured with colour.analyze really is warmer
    const QJsonObject ap = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}, {Q("apply"), true}, {Q("origin"), Q("agent:colourist")}});
    QVERIFY(ap.value(Q("applied")).toBool());
    QVERIFY(ap.value(Q("verified")).toBool());
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt() + 1);
    QCOMPARE(ok(Q("history.list")).value(Q("entries")).toArray().last().toObject().value(Q("actor")).toString(), Q("agent:colourist"));
    QVERIFY(std::fabs(ok(Q("item.get"), {{Q("itemId"), item}}).value(Q("color")).toObject().value(Q("primaries")).toObject().value(Q("temperature")).toDouble() - 0.14) < 1e-9);
    const double cast1 = castOf(ok(Q("colour.analyze"), {{Q("itemId"), item}, {Q("samples"), 2}}));
    QVERIFY2(cast1 > cast0 + 0.08, qPrintable(QStringLiteral("cast %1 -> %2").arg(cast0).arg(cast1)));
    const double measuredCast = ap.value(Q("items")).toArray().at(0).toObject().value(Q("measured")).toObject().value(Q("castTemperature")).toDouble();
    QVERIFY(std::fabs(measuredCast - cast1) < 0.002);

    // layering: a second look stacks on the first (temperature adds); replace starts over
    const QJsonObject stacked = ok(Q("look.apply"), {{Q("look"), Q("golden-hour")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 1}}).value(Q("items")).toArray().at(0).toObject();
    QVERIFY(std::fabs(stacked.value(Q("proposedPrimaries")).toObject().value(Q("temperature")).toDouble() - 0.42) < 1e-9);
    const QJsonObject replaced = ok(Q("look.apply"), {{Q("look"), Q("golden-hour")}, {Q("replace"), true}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 1}}).value(Q("items")).toArray().at(0).toObject();
    QVERIFY(std::fabs(replaced.value(Q("proposedPrimaries")).toObject().value(Q("temperature")).toDouble() - 0.28) < 1e-9);
    QCOMPARE(replaced.value(Q("currentPrimaries")).toObject().value(Q("temperature")).toDouble(), 0.14);

    // undo goes back to the ungraded clip; a cool look measures cooler
    ok(Q("history.undo"));
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), item}}).contains(Q("color")));
    const QJsonObject cool = ok(Q("look.apply"), {{Q("look"), Q("night-cool")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}}).value(Q("items")).toArray().at(0).toObject();
    QVERIFY(cool.value(Q("predicted")).toObject().value(Q("castTemperature")).toDouble() < cast0 - 0.1);
    QVERIFY(cool.value(Q("predicted")).toObject().value(Q("exposureStops")).toDouble() < cool.value(Q("before")).toObject().value(Q("exposureStops")).toDouble() - 0.3);

    // guards: a limit the look cannot meet blocks the write, names the rule, and proposes a gentler intensity
    const QJsonObject tight = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}, {Q("guards"), QJsonObject{{Q("maxLiftMaster"), 0.001}}}, {Q("apply"), true}});
    QVERIFY(!tight.value(Q("ok")).toBool());
    QVERIFY(!tight.value(Q("applied")).toBool());
    const QJsonObject tc = tight.value(Q("items")).toArray().at(0).toObject();
    QStringList codes;
    for (const QJsonValue& v : tc.value(Q("check")).toObject().value(Q("violations")).toArray()) codes << v.toObject().value(Q("code")).toString();
    QVERIFY2(codes.contains(Q("maxLiftMaster")), qPrintable(codes.join(Q(","))));
    QVERIFY(tc.contains(Q("suggestedIntensity")));
    QVERIFY(!ok(Q("item.get"), {{Q("itemId"), item}}).contains(Q("color")));
    const QJsonObject forced = ok(Q("look.apply"), {{Q("look"), Q("warm-documentary")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}, {Q("guards"), QJsonObject{{Q("maxLiftMaster"), 0.001}}}, {Q("apply"), true}, {Q("force"), true}});
    QVERIFY(forced.value(Q("applied")).toBool());
    ok(Q("history.undo"));
    const QJsonObject chroma = ok(Q("look.apply"), {{Q("look"), Q("golden-hour")}, {Q("itemIds"), QJsonArray{item}}, {Q("samples"), 2}, {Q("guards"), QJsonObject{{Q("maxChromaP95"), 0.0005}}}});
    QStringList codes2;
    for (const QJsonValue& v : chroma.value(Q("items")).toArray().at(0).toObject().value(Q("check")).toObject().value(Q("violations")).toArray()) codes2 << v.toObject().value(Q("code")).toString();
    QVERIFY2(codes2.contains(Q("maxChromaP95")), qPrintable(codes2.join(Q(","))));

    // errors
    QCOMPARE(raw(Q("look.apply"), {{Q("look"), Q("sepia")}, {Q("itemIds"), QJsonArray{item}}}).error.code, Q("invalid_params")); // not in the enum
    QCOMPARE(raw(Q("look.apply"), {{Q("look"), Q("pastel")}, {Q("itemIds"), QJsonArray{Q("itm_nope")}}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("look.apply"), {{Q("look"), Q("pastel")}, {Q("itemIds"), QJsonArray{}}}).error.code, Q("invalid_params"));
    const QString text = ok(Q("text.add"), {{Q("text"), Q("hi")}, {Q("startFrame"), 0}, {Q("durationFrames"), 10}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    QCOMPARE(raw(Q("look.apply"), {{Q("look"), Q("pastel")}, {Q("itemIds"), QJsonArray{text}}}).error.code, Q("invalid_params"));
    qInfo().noquote() << QStringLiteral("style: warm-documentary cast %1 -> %2 (predicted %3, half intensity %4)").arg(cast0, 0, 'f', 3).arg(cast1, 0, 'f', 3).arg(pFull, 0, 'f', 3).arg(pHalf, 0, 'f', 3);
  }

  void pacingProfilesOnASyntheticTimeline() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{story_}}});
    const QString story = assetNamed(Q("story.mp4"));
    for (int i = 0; i < 12; ++i) addClip(story, i * 25, 25, 0); // twelve 1 s shots, cuts every 25 frames
    const QJsonObject doc = ok(Q("pacing.check"), {{Q("profile"), Q("documentary")}});
    QVERIFY(!doc.value(Q("pass")).toBool());
    QCOMPARE(rulesOf(doc), QStringList{Q("asl_too_fast")});
    QCOMPARE(doc.value(Q("measured")).toObject().value(Q("aslSec")).toDouble(), 1.0);
    QCOMPARE(doc.value(Q("measured")).toObject().value(Q("shotCount")).toInt(), 12);
    QCOMPARE(doc.value(Q("violations")).toArray().at(0).toObject().value(Q("limit")).toDouble(), 4.0);
    QCOMPARE(doc.value(Q("profile")).toObject().value(Q("name")).toString(), Q("documentary"));
    QCOMPARE(rulesOf(ok(Q("pacing.check"), {{Q("profile"), Q("music-video")}})), QStringList{Q("asl_too_fast")}); // 1.0 s < 1.2 s
    // no music on the timeline: the beat share is not checked, and the note says why
    const QJsonObject social = ok(Q("pacing.check"), {{Q("profile"), Q("social-short")}});
    QVERIFY2(social.value(Q("pass")).toBool(), qPrintable(QString::fromUtf8(QJsonDocument(social).toJson())));
    QVERIFY(social.value(Q("measured")).toObject().value(Q("cutsOnBeatPct")).isNull());
    QVERIFY(social.value(Q("notes")).toArray().size() == 1);
    // with a grid the share is measured: every cut on a beat, then every cut 10 frames off
    QVector<int> grid, offGrid;
    for (int f = 0; f <= 300; f += 25) {
      grid << f;
      offGrid << f + 10;
    }
    QJsonArray onBeats, offBeats;
    for (int f : grid) onBeats.append(f);
    for (int f : offGrid) offBeats.append(f);
    const QJsonObject on = ok(Q("pacing.check"), {{Q("profile"), Q("social-short")}, {Q("beatFrames"), onBeats}});
    QVERIFY(on.value(Q("pass")).toBool());
    QCOMPARE(on.value(Q("measured")).toObject().value(Q("cutsOnBeatPct")).toDouble(), 100.0);
    const QJsonObject off = ok(Q("pacing.check"), {{Q("profile"), Q("social-short")}, {Q("beatFrames"), offBeats}});
    QCOMPARE(rulesOf(off), QStringList{Q("beat_share")});
    QCOMPARE(off.value(Q("violations")).toArray().at(0).toObject().value(Q("limit")).toDouble(), 30.0);
    QVERIFY(raw(Q("pacing.check"), {{Q("profile"), Q("interview")}}).ok);
    QCOMPARE(raw(Q("pacing.check"), {{Q("profile"), Q("nope")}}).error.code, Q("invalid_params")); // not in the enum
    QCOMPARE(Engine().call(Q("pacing.check"), {{Q("profile"), Q("trailer")}}).error.code, Q("no_session"));
  }

  void mixProfilesApplyTheirNumbers() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_}}});
    ok(Q("clip.add"), {{Q("assetId"), firstAsset(Q("video"))}});
    ok(Q("audio.add"), {{Q("assetId"), firstAsset(Q("audio"))}});
    QString videoTrack, audioTrack;
    for (const QJsonValue& t : ok(Q("timeline.get"), {{Q("mode"), Q("summary")}}).value(Q("tracks")).toArray()) {
      const QJsonObject to = t.toObject();
      if (to.value(Q("items")).toArray().isEmpty()) continue;
      if (to.value(Q("kind")).toString() == Q("video")) videoTrack = to.value(Q("id")).toString();
      if (to.value(Q("kind")).toString() == Q("audio")) audioTrack = to.value(Q("id")).toString();
    }
    QVERIFY(!videoTrack.isEmpty() && !audioTrack.isEmpty());

    // dry run: the numbers handed to audio.duck and audio.normalize are the profile's, and nothing is written
    const QJsonObject hist0 = ok(Q("history.list"));
    for (const char* name : {"podcast", "streaming", "cinematic", "social"}) {
      const QJsonObject prof = ok(Q("style.list"), {{Q("kind"), Q("mix")}, {Q("name"), Q(name)}}).value(Q("items")).toArray().at(0).toObject();
      const QJsonObject r = ok(Q("mix.applyProfile"), {{Q("profile"), Q(name)}, {Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}});
      QVERIFY(!r.value(Q("applied")).toBool());
      const QJsonObject duck = r.value(Q("ducking")).toObject(), norm = r.value(Q("normalize")).toObject();
      QCOMPARE(duck.value(Q("targetMarginDb")).toDouble(), prof.value(Q("duckMarginDb")).toDouble());
      QCOMPARE(duck.value(Q("attackMs")).toDouble(), prof.value(Q("attackMs")).toDouble());
      QCOMPARE(duck.value(Q("releaseMs")).toDouble(), prof.value(Q("releaseMs")).toDouble());
      QCOMPARE(norm.value(Q("target")).toDouble(), prof.value(Q("targetLufs")).toDouble());
      QCOMPARE(norm.value(Q("truePeakCeiling")).toDouble(), prof.value(Q("truePeakCeilingDb")).toDouble());
      const QJsonObject pr = norm.value(Q("proposal")).toObject();
      QVERIFY(norm.value(Q("proposal")).isObject());
      QVERIFY2(std::fabs(pr.value(Q("predictedLufs")).toDouble() - prof.value(Q("targetLufs")).toDouble()) < 0.1 || pr.value(Q("limitedByTruePeak")).toBool(), name);
      QCOMPARE(r.value(Q("profile")).toObject().value(Q("targetLufs")).toDouble(), prof.value(Q("targetLufs")).toDouble());
    }
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt());
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
    // the roles can be guessed from the tracks too
    QVERIFY(!ok(Q("mix.applyProfile"), {{Q("profile"), Q("podcast")}}).value(Q("ducking")).toObject().contains(Q("skipped")));
    QCOMPARE(raw(Q("mix.applyProfile"), {{Q("profile"), Q("loudest")}}).error.code, Q("invalid_params"));

    // apply: the duck, then the loudness, together ONE undo step; the mix lands on the target
    const QJsonObject ap = ok(Q("mix.applyProfile"), {{Q("profile"), Q("podcast")}, {Q("speechTrack"), videoTrack}, {Q("musicTrack"), audioTrack}, {Q("apply"), true}, {Q("origin"), Q("agent:audio")}});
    QVERIFY(ap.value(Q("applied")).toBool());
    QCOMPARE(ap.value(Q("undoSteps")).toInt(), 1);
    QCOMPARE(ap.value(Q("writes")).toInt(), 2);
    QCOMPARE(ok(Q("history.list")).value(Q("total")).toInt(), hist0.value(Q("total")).toInt() + 1);
    const double lufs = ok(Q("audio.measure")).value(Q("integratedLufs")).toDouble();
    const bool limited = ap.value(Q("normalize")).toObject().value(Q("proposal")).toObject().value(Q("limitedByTruePeak")).toBool();
    QVERIFY2(std::fabs(lufs + 16.0) < 0.6 || limited, qPrintable(QString::number(lufs)));
    QVERIFY(ap.contains(Q("check")));
    const QJsonObject margin = ap.value(Q("ducking")).toObject().value(Q("after")).toObject();
    QVERIFY2(std::fabs(margin.value(Q("marginDb")).toDouble() - 12.0) < 1.5, qPrintable(QString::number(margin.value(Q("marginDb")).toDouble())));
    qInfo().noquote() << QStringLiteral("mix: podcast -> %1 LUFS (target -16), speech margin %2 dB (target 12)").arg(lufs, 0, 'f', 2).arg(margin.value(Q("marginDb")).toDouble(), 0, 'f', 2);
    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1); // one undo restores the untouched mix
    QVERIFY(ok(Q("mixer.get")).value(Q("mixer")).isNull());
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

  // ---- background analysis jobs ----
  void jobsCommands() {
    e_->jobs().setCacheDir(dir_.filePath(QStringLiteral("jobs-cache")));
    QVERIFY(ok(Q("jobs.policy"), {{Q("autoPause"), false}}).contains(Q("paused"))); // this machine's battery / VRAM must not stall the test
    auto fake = std::make_shared<FakeAnalyzer>();
    e_->jobs().registerAnalyzer(fake);
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_, music_}}});
    const QString music = firstAsset(Q("audio")), video = firstAsset(Q("video"));
    QVERIFY(!music.isEmpty() && !video.isEmpty());

    const QJsonObject list = ok(Q("jobs.list"));
    QStringList ids;
    for (const QJsonValue& a : list.value(Q("analyzers")).toArray()) ids << a.toObject().value(Q("id")).toString();
    QVERIFY(ids.contains(Q("hands")) && ids.contains(Q("waveform")) && ids.contains(Q("fake")));
    QVERIFY(!list.value(Q("paused")).toBool());

    // the non-ML analyzer through the commands: wait for it, then the same request is a cache hit
    const QJsonObject first = ok(Q("jobs.enqueue"), {{Q("analyzer"), Q("waveform")}, {Q("assetId"), music}, {Q("params"), QJsonObject{{Q("buckets"), 32}}}, {Q("wait"), true}, {Q("includeResult"), true}});
    QCOMPARE(first.value(Q("status")).toString(), Q("done"));
    QVERIFY(!first.value(Q("cached")).toBool());
    QVERIFY(first.value(Q("result")).toObject().value(Q("buckets")).toInt() > 0);
    const QJsonObject second = ok(Q("jobs.enqueue"), {{Q("analyzer"), Q("waveform")}, {Q("assetId"), music}, {Q("params"), QJsonObject{{Q("buckets"), 32}}}});
    QVERIFY(second.value(Q("cached")).toBool());
    QCOMPARE(second.value(Q("status")).toString(), Q("done"));
    const QJsonObject got = ok(Q("analysis.get"), {{Q("analyzer"), Q("waveform")}, {Q("assetId"), music}, {Q("params"), QJsonObject{{Q("buckets"), 32}}}});
    QCOMPARE(got.value(Q("status")).toString(), Q("done"));
    QCOMPARE(got.value(Q("result")).toObject(), first.value(Q("result")).toObject());
    QCOMPARE(ok(Q("analysis.get"), {{Q("analyzer"), Q("waveform")}, {Q("assetId"), music}, {Q("params"), QJsonObject{{Q("buckets"), 64}}}}).value(Q("status")).toString(), Q("none"));

    // an item resolves to its asset; pause / queue / cancel / resume
    ok(Q("clip.add"), {{Q("assetId"), video}, {Q("startFrame"), 0}});
    QString itemId;
    for (const QJsonValue& i : items()) itemId = i.toObject().value(Q("id")).toString();
    QVERIFY(!itemId.isEmpty());
    QCOMPARE(ok(Q("jobs.pause"), {{Q("reason"), Q("llm generating")}}).value(Q("pauseReasons")).toArray().at(0).toString(), Q("manual:llm generating"));
    const QJsonObject queued = ok(Q("jobs.enqueue"), {{Q("analyzer"), Q("fake")}, {Q("itemId"), itemId}});
    QCOMPARE(queued.value(Q("status")).toString(), Q("queued"));
    QVERIFY(queued.value(Q("note")).toString().contains(Q("paused")));
    QCOMPARE(ok(Q("jobs.list"), {{Q("state"), Q("queued")}}).value(Q("jobs")).toArray().size(), 1);
    QCOMPARE(ok(Q("analysis.get"), {{Q("analyzer"), Q("fake")}, {Q("itemId"), itemId}}).value(Q("status")).toString(), Q("queued"));
    const QJsonObject cancelled = ok(Q("jobs.cancel"), {{Q("jobId"), queued.value(Q("jobId")).toString()}});
    QCOMPARE(cancelled.value(Q("state")).toString(), Q("cancelled"));
    QCOMPARE(fake->calls.load(), 0);
    const QJsonObject again = ok(Q("jobs.enqueue"), {{Q("analyzer"), Q("fake")}, {Q("itemId"), itemId}});
    QJsonObject resumed = ok(Q("jobs.resume"));
    QVERIFY(!resumed.value(Q("paused")).toBool());
    const QJsonObject done = ok(Q("analysis.get"), {{Q("analyzer"), Q("fake")}, {Q("itemId"), itemId}, {Q("wait"), true}});
    QCOMPARE(done.value(Q("status")).toString(), Q("done"));
    QCOMPARE(fake->calls.load(), 1);
    QVERIFY(!again.value(Q("jobId")).toString().isEmpty());

    // errors are values
    QCOMPARE(raw(Q("jobs.enqueue"), {{Q("analyzer"), Q("nope")}, {Q("assetId"), music}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("jobs.enqueue"), {{Q("analyzer"), Q("fake")}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("jobs.enqueue"), {{Q("analyzer"), Q("fake")}, {Q("assetId"), Q("ast_nope")}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("jobs.cancel"), {{Q("jobId"), Q("job_nope")}}).error.code, Q("not_found"));

    // hands: a handful of tiny frames when ONNX Runtime and the models are here, a clean `unavailable` otherwise
    const Result hands = raw(Q("analysis.hands.track"), {{Q("itemId"), itemId}, {Q("fps"), 2}, {Q("wait"), true}, {Q("timeoutMs"), 120000}});
    if (!hands.ok) {
      QCOMPARE(hands.error.code, Q("unavailable"));
    } else {
      const QJsonObject h = hands.value.toObject();
      QCOMPARE(h.value(Q("status")).toString(), Q("done"));
      QCOMPARE(h.value(Q("result")).toObject().value(Q("frameCount")).toInt(), 4); // 2 s at 2 fps
      QVERIFY(ok(Q("analysis.hands.track"), {{Q("itemId"), itemId}, {Q("fps"), 2}}).value(Q("cached")).toBool());
    }
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
    // The export renderer needs a working D3D11 QRhi; machines whose D3D11 stack cannot create
    // one fail before the first frame. Same boundary the frame/analysis tests above skip on.
    if (done.value(Q("state")).toString() == Q("failed")
        && done.value(Q("error")).toString().contains(Q("D3D11"))) {
      QSKIP("no D3D11 device for the offscreen renderer");
    }
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

    // policy wiring: override exportEncoder to software
    ok(Q("system.overrides.set"), {{Q("key"), Q("exportEncoder")}, {Q("value"), Q("software")}});
    const QJsonObject stPol = ok(Q("export.start"), {{Q("out"), dir_.filePath(Q("pol.mp4"))}, {Q("quality"), Q("draft")}, {Q("range"), QJsonArray{0, 2}}});
    const QJsonObject encDecPol = stPol.value(Q("encoderDecision")).toObject();
    QCOMPARE(encDecPol.value(Q("value")).toString(), Q("software"));
    QCOMPARE(encDecPol.value(Q("source")).toString(), Q("policy"));
    ok(Q("export.status"), {{Q("jobId"), stPol.value(Q("jobId")).toString()}, {Q("waitMs"), 90000}});

    // explicit hardware/encoder arg gives source == "caller"
    const QJsonObject stCaller = ok(Q("export.start"), {{Q("out"), dir_.filePath(Q("caller.mp4"))}, {Q("quality"), Q("draft")}, {Q("hw"), false}, {Q("range"), QJsonArray{0, 2}}});
    const QJsonObject encDecCaller = stCaller.value(Q("encoderDecision")).toObject();
    QCOMPARE(encDecCaller.value(Q("source")).toString(), Q("caller"));
    ok(Q("export.status"), {{Q("jobId"), stCaller.value(Q("jobId")).toString()}, {Q("waitMs"), 90000}});

    // restore the override
    ok(Q("system.overrides.set"), {{Q("key"), Q("exportEncoder")}, {Q("value"), QJsonValue(QJsonValue::Null)}});

    QCOMPARE(ok(Q("export.status")).value(Q("jobs")).toArray().size(), 4);
  }
  // ---- GPU effects and transitions: every mutation is one undo step ----
  void effectsAndTransitions() {
    // the catalogue
    const QJsonArray all = ok(Q("effect.list")).value(Q("effects")).toArray();
    QVERIFY(all.size() >= 5);
    const QJsonArray filters = ok(Q("effect.list"), {{Q("kind"), Q("filter")}}).value(Q("effects")).toArray();
    const QJsonArray transitions = ok(Q("effect.list"), {{Q("kind"), Q("transition")}}).value(Q("effects")).toArray();
    QCOMPARE(filters.size() + transitions.size(), all.size());
    QVERIFY2(filters.size() >= 25, qPrintable(Q("filters: ") + QString::number(filters.size())));
    QVERIFY(ok(Q("effect.list"), {{Q("category"), Q("wipe")}}).value(Q("effects")).toArray().size() >= 1);
    QVERIFY2(transitions.size() >= 100, qPrintable(Q("transitions: ") + QString::number(transitions.size()))); // the gl-transitions port
    int glCount = 0;
    for (const QJsonValue& v : transitions) glCount += v.toObject().value(Q("id")).toString().startsWith(Q("gl_")) ? 1 : 0;
    QVERIFY2(glCount >= 100, qPrintable(QString::number(glCount)));
    QJsonObject vignette;
    for (const QJsonValue& v : filters)
      if (v.toObject().value(Q("id")).toString() == Q("vignette")) vignette = v.toObject();
    QCOMPARE(vignette.value(Q("params")).toArray().at(0).toObject().value(Q("name")).toString(), Q("amount"));
    QVERIFY(!vignette.value(Q("whenToUse")).toArray().isEmpty());

    // two text clips butted together on the text track
    const auto addText = [&](const char* text, int start) {
      return ok(Q("text.add"), {{Q("text"), Q(text)}, {Q("startFrame"), start}, {Q("durationFrames"), 20}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    };
    const QString a = addText("A", 0), b = addText("B", 20);
    QVERIFY(!a.isEmpty() && !b.isEmpty());
    const auto stack = [&]() { return ok(Q("item.get"), {{Q("itemId"), a}}).value(Q("fx")).toArray(); };
    const auto undo = [&]() { QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1); };
    const auto redo = [&]() { QCOMPARE(ok(Q("history.redo")).value(Q("steps")).toInt(), 1); };
    QVERIFY(stack().isEmpty());

    // effect.add (params merge over defaults), effect.set, effect.keyframe, effect.reorder, effect.remove
    const QJsonObject added = ok(Q("effect.add"), {{Q("itemId"), a}, {Q("effectId"), Q("vignette")}, {Q("params"), QJsonObject{{Q("amount"), 0.4}}}});
    const QString vig = added.value(Q("fxId")).toString();
    QVERIFY(!vig.isEmpty());
    QCOMPARE(added.value(Q("fx")).toArray().size(), 1);
    QCOMPARE(added.value(Q("inverse")).toArray().at(0).toObject().value(Q("type")).toString(), Q("item.update"));
    const QString blur = ok(Q("effect.add"), {{Q("itemId"), a}, {Q("effectId"), Q("gaussian_blur")}, {Q("index"), 0}}).value(Q("fxId")).toString();
    QCOMPARE(stack().at(0).toObject().value(Q("id")).toString(), blur); // index 0: applied first
    ok(Q("effect.reorder"), {{Q("itemId"), a}, {Q("fxId"), blur}, {Q("index"), 1}});
    QCOMPARE(stack().at(1).toObject().value(Q("id")).toString(), blur);
    ok(Q("effect.set"), {{Q("itemId"), a}, {Q("fxId"), vig}, {Q("params"), QJsonObject{{Q("radius"), 0.2}, {Q("amount"), 7}}}, {Q("enabled"), false}});
    const QJsonObject v0 = stack().at(0).toObject();
    QCOMPARE(v0.value(Q("enabled")).toBool(), false);
    QCOMPARE(v0.value(Q("params")).toObject().value(Q("radius")).toDouble(), 0.2);
    QCOMPARE(v0.value(Q("params")).toObject().value(Q("amount")).toDouble(), 1.0); // clamped to the param's range
    ok(Q("effect.set"), {{Q("itemId"), a}, {Q("fxId"), vig}, {Q("params"), QJsonObject{{Q("radius"), QJsonValue::Null}}}});
    QVERIFY(!stack().at(0).toObject().value(Q("params")).toObject().contains(Q("radius"))); // null resets to the default
    ok(Q("effect.keyframe"), {{Q("itemId"), a}, {Q("fxId"), blur}, {Q("param"), Q("radius")}, {Q("t"), 10}, {Q("v"), 20}, {Q("ease"), Q("easeIn")}});
    ok(Q("effect.keyframe"), {{Q("itemId"), a}, {Q("fxId"), blur}, {Q("param"), Q("radius")}, {Q("t"), 0}, {Q("v"), 0}});
    ok(Q("effect.keyframe"), {{Q("itemId"), a}, {Q("fxId"), blur}, {Q("param"), Q("radius")}, {Q("t"), 10}, {Q("v"), 30}}); // same frame: replaced
    const QJsonArray kf = stack().at(1).toObject().value(Q("keyframes")).toObject().value(Q("radius")).toArray();
    QCOMPARE(kf.size(), 2);
    QCOMPARE(kf.at(0).toObject().value(Q("frame")).toInt(), 0); // sorted by frame
    QCOMPARE(kf.at(1).toObject().value(Q("value")).toDouble(), 30.0);

    // errors are values and change nothing
    QCOMPARE(raw(Q("effect.add"), {{Q("itemId"), a}, {Q("effectId"), Q("crossfade")}}).error.code, Q("invalid_params")); // a transition is not a filter
    QCOMPARE(raw(Q("effect.add"), {{Q("itemId"), a}, {Q("effectId"), Q("nope")}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("effect.add"), {{Q("itemId"), a}, {Q("effectId"), Q("vignette")}, {Q("params"), QJsonObject{{Q("bogus"), 1}}}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("effect.keyframe"), {{Q("itemId"), a}, {Q("fxId"), blur}, {Q("param"), Q("bogus")}, {Q("t"), 0}, {Q("v"), 1}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("effect.set"), {{Q("itemId"), a}, {Q("fxId"), Q("fx_missing")}, {Q("enabled"), true}}).error.code, Q("not_found"));
    QCOMPARE(raw(Q("effect.set"), {{Q("itemId"), a}, {Q("fxId"), vig}}).error.code, Q("invalid_params")); // nothing to change
    QCOMPARE(stack().size(), 2);

    // each of the 8 successful edits above is one undo step, and redo brings it back
    undo(); // keyframe (replace)
    QCOMPARE(stack().at(1).toObject().value(Q("keyframes")).toObject().value(Q("radius")).toArray().at(1).toObject().value(Q("value")).toDouble(), 20.0);
    redo();
    for (int i = 0; i < 3; ++i) undo(); // the three keyframes
    QVERIFY(!stack().at(1).toObject().contains(Q("keyframes")));
    undo(); // reset radius
    QCOMPARE(stack().at(0).toObject().value(Q("params")).toObject().value(Q("radius")).toDouble(), 0.2);
    undo(); // set
    QCOMPARE(stack().at(0).toObject().value(Q("enabled")).toBool(), true);
    undo(); // reorder
    QCOMPARE(stack().at(0).toObject().value(Q("id")).toString(), blur);
    undo(); // second add
    QCOMPARE(stack().size(), 1);
    undo(); // first add
    QVERIFY(stack().isEmpty());
    for (int i = 0; i < 8; ++i) redo();
    QCOMPARE(stack().size(), 2);
    ok(Q("effect.remove"), {{Q("itemId"), a}, {Q("fxId"), blur}});
    QCOMPARE(stack().size(), 1);
    undo();
    QCOMPARE(stack().size(), 2);

    // transitions
    QCOMPARE(raw(Q("transition.add"), {{Q("itemId"), a}, {Q("effectId"), Q("vignette")}, {Q("duration"), 8}}).error.code, Q("invalid_params")); // a filter is not a transition
    QCOMPARE(raw(Q("transition.add"), {{Q("itemId"), a}, {Q("effectId"), Q("crossfade")}, {Q("duration"), 41}}).error.code, Q("invalid_params")); // longer than 2 x 20 frames
    QCOMPARE(raw(Q("transition.add"), {{Q("itemId"), b}, {Q("effectId"), Q("crossfade")}, {Q("duration"), 8}}).error.code, Q("invalid_params")); // nothing after B
    QCOMPARE(raw(Q("transition.add"), {{Q("effectId"), Q("crossfade")}, {Q("duration"), 8}}).error.code, Q("invalid_params")); // no cut named
    const QJsonObject tadd = ok(Q("transition.add"), {{Q("itemId"), a}, {Q("effectId"), Q("wipe")}, {Q("duration"), 10}, {Q("align"), Q("start")}, {Q("params"), QJsonObject{{Q("softness"), 0.3}}}});
    const QString tr = tadd.value(Q("transitionIds")).toArray().at(0).toString();
    QVERIFY(!tr.isEmpty());
    QJsonObject listed = ok(Q("transition.list")).value(Q("transitions")).toArray().at(0).toObject();
    QCOMPARE(listed.value(Q("fromItemId")).toString(), a);
    QCOMPARE(listed.value(Q("toItemId")).toString(), b);
    QCOMPARE(listed.value(Q("valid")).toBool(), true);
    QCOMPARE(listed.value(Q("startFrame")).toInt(), 20);
    QCOMPARE(listed.value(Q("endFrame")).toInt(), 30);
    QCOMPARE(tadd.value(Q("inverse")).toArray().at(0).toObject().value(Q("type")).toString(), Q("transitions.set"));
    QCOMPARE(raw(Q("transition.add"), {{Q("itemId"), a}, {Q("effectId"), Q("crossfade")}, {Q("duration"), 8}}).error.code, Q("invalid_params")); // the cut has one already

    ok(Q("transition.set"), {{Q("transitionId"), tr}, {Q("effectId"), Q("crossfade")}, {Q("duration"), 6}, {Q("align"), Q("center")}});
    listed = ok(Q("transition.list")).value(Q("transitions")).toArray().at(0).toObject();
    QCOMPARE(listed.value(Q("effectId")).toString(), Q("crossfade"));
    QCOMPARE(listed.value(Q("startFrame")).toInt(), 17); // centre: 3 frames before the cut
    QVERIFY(listed.value(Q("params")).toObject().isEmpty()); // a new effect starts from its own defaults
    QCOMPARE(raw(Q("transition.set"), {{Q("transitionId"), tr}, {Q("duration"), 999}}).error.code, Q("invalid_params"));
    QCOMPARE(raw(Q("transition.set"), {{Q("transitionId"), tr}, {Q("effectId"), Q("gl_luma")}, {Q("params"), QJsonObject{{Q("luma"), Q("unknown_texture")}}}}).error.code, Q("invalid_params"));
    undo();
    QCOMPARE(ok(Q("transition.list")).value(Q("transitions")).toArray().at(0).toObject().value(Q("effectId")).toString(), Q("wipe"));
    redo();

    // it saves with the project and loads again
    const QString file = dir_.filePath(Q("fx.sfproj"));
    ok(Q("project.saveAs"), {{Q("path"), file}});
    ok(Q("project.open"), {{Q("path"), file}});
    QCOMPARE(ok(Q("transition.list")).value(Q("transitions")).toArray().size(), 1);
    QCOMPARE(stack().size(), 2);

    ok(Q("transition.remove"), {{Q("transitionId"), tr}});
    QVERIFY(ok(Q("transition.list")).value(Q("transitions")).toArray().isEmpty());
    undo();
    QCOMPARE(ok(Q("transition.list")).value(Q("transitions")).toArray().size(), 1);
    redo();
    QCOMPARE(raw(Q("transition.remove"), {{Q("transitionId"), tr}}).error.code, Q("not_found"));
    // a whole track at once
    const QString track = ok(Q("item.get"), {{Q("itemId"), a}}).value(Q("trackId")).toString();
    QCOMPARE(ok(Q("transition.add"), {{Q("trackId"), track}, {Q("effectId"), Q("crossfade")}, {Q("duration"), 10}}).value(Q("transitionIds")).toArray().size(), 1);
  }

  // mask.trackHands on a clip without hands: no tracked samples, one gap covering the clip,
  // an opacity keyframe at 0, and one undo removes the added fx.
  void maskTrackHandsNoHands() {
    ok(Q("jobs.policy"), {{Q("autoPause"), false}});
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    ok(Q("clip.add"), {{Q("assetId"), video}, {Q("startFrame"), 0}});
    QString itemId;
    for (const QJsonValue& i : items()) itemId = i.toObject().value(Q("id")).toString();
    QVERIFY(!itemId.isEmpty());

    const Result track = raw(Q("mask.trackHands"), {{Q("itemId"), itemId}, {Q("wait"), true}});
    if (!track.ok && track.error.code == Q("unavailable")) {
      QSKIP("hands analyzer unavailable (ONNX models not present on this machine)");
    }
    QVERIFY2(track.ok, qPrintable(track.error.message));
    const QJsonObject r = track.value.toObject();
    QCOMPARE(r.value(Q("trackedSamples")).toInt(), 0);
    const QJsonArray gaps = r.value(Q("gaps")).toArray();
    QCOMPARE(gaps.size(), 1);
    QCOMPARE(gaps.at(0).toObject().value(Q("startSec")).toDouble(), 0.0);
    QVERIFY(gaps.at(0).toObject().value(Q("endSec")).toDouble() > 0.0);

    const QJsonArray fx = ok(Q("item.get"), {{Q("itemId"), itemId}}).value(Q("fx")).toArray();
    QCOMPARE(fx.size(), 1);
    QCOMPARE(fx.at(0).toObject().value(Q("effectId")).toString(), Q("polygon_mask"));
    const QJsonArray kf = fx.at(0).toObject().value(Q("keyframes")).toObject().value(Q("opacity")).toArray();
    QCOMPARE(kf.size(), 1);
    QCOMPARE(kf.at(0).toObject().value(Q("frame")).toInt(), 0);
    QCOMPARE(kf.at(0).toObject().value(Q("value")).toDouble(), 0.0);

    QCOMPARE(ok(Q("history.undo")).value(Q("steps")).toInt(), 1);
    QVERIFY(ok(Q("item.get"), {{Q("itemId"), itemId}}).value(Q("fx")).toArray().isEmpty());
  }

  // safety.check: flashes, text legibility, safe zones. Shapes and text only (no media), tiny renders (64 px wide analysis).
  void safetyCheck() {
    const auto fresh = [&](int w, int h) { ok(Q("project.new"), {{Q("name"), Q("Safety")}, {Q("width"), w}, {Q("height"), h}, {Q("fps"), 30}}); };
    const auto kinds = [](const QJsonArray& a) {
      QStringList k;
      for (const QJsonValue& v : a) k << v.toObject().value(Q("kind")).toString();
      return k;
    };
    // `period` frames white (or `fill`), `period` frames black, `count` times; the shape covers `frac` of the canvas width and height
    const auto flasher = [&](const QString& fill, int period, int count, double frac = 1.0) {
      for (int i = 0; i < count; ++i)
        ok(Q("shape.add"), {{Q("shape"), Q("rect")}, {Q("fill"), fill}, {Q("width"), 320 * frac}, {Q("height"), 180 * frac}, {Q("startFrame"), 2 * i * period}, {Q("durationFrames"), period}});
    };
    const QJsonObject photo{{Q("checks"), QJsonArray{Q("photosensitive")}}, {Q("analysisWidth"), 64}};
    {
      fresh(320, 180);
      flasher(Q("#ffffff"), 3, 10);
      const Result probe = raw(Q("safety.check"), photo);
      if (!probe.ok && probe.error.code == Q("unavailable") && probe.error.message.contains(Q("renderer"))) QSKIP("no D3D11 device for the offscreen renderer");
      QVERIFY2(probe.ok, qPrintable(probe.error.message));
      const QJsonObject r = probe.value.toObject();
      // The flash/contrast rules render through a D3D11 QRhi; on devices without one the rules
      // land in `skipped` with the renderer error. Same boundary the tests above skip on.
      for (const QJsonValue& sk : r.value(Q("summary")).toObject().value(Q("skipped")).toArray())
        if (sk.toString().contains(Q("D3D11"))) QSKIP("no D3D11 device for the offscreen renderer");
      QVERIFY(r.value(Q("summary")).toObject().value(Q("skipped")).toArray().isEmpty());
      QVERIFY2(kinds(r.value(Q("violations")).toArray()).contains(Q("photosensitive_general_flash")), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
      QVERIFY(!r.value(Q("pass")).toBool());
      const QJsonObject v = r.value(Q("violations")).toArray().at(0).toObject();
      QVERIFY(v.contains(Q("measured")) && v.contains(Q("threshold")) && v.contains(Q("fix")) && v.contains(Q("startSec")) && v.contains(Q("endSec")));
      const QJsonObject g = r.value(Q("photosensitive")).toObject().value(Q("general")).toObject();
      QVERIFY(g.value(Q("worstWindow")).toObject().value(Q("flashes")).toInt() > 3);
      QVERIFY(g.value(Q("flashesPerSecondWindow")).toArray().size() >= 1);
      // timeline.check carries it only when asked
      ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
      ok(Q("clip.add"), {{Q("assetId"), firstAsset(Q("video"))}, {Q("startFrame"), 100}}); // timeline.check needs a story track
      QVERIFY(!ok(Q("timeline.check")).contains(Q("safety")));
      const QJsonObject tc = ok(Q("timeline.check"), {{Q("includeSafety"), true}, {Q("safetyChecks"), QJsonArray{Q("photosensitive")}}});
      QVERIFY(tc.contains(Q("safety")));
      QVERIFY(!tc.value(Q("pass")).toBool());
      QVERIFY(rulesOf(tc).contains(Q("photosensitive_general_flash")));
    }
    { // slow: 15 frames per state is 1 flash a second
      fresh(320, 180);
      flasher(Q("#ffffff"), 15, 3);
      const QJsonObject r = ok(Q("safety.check"), photo);
      QVERIFY2(r.value(Q("violations")).toArray().isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
    }
    { // red
      fresh(320, 180);
      flasher(Q("#ff0000"), 3, 10);
      const QJsonObject r = ok(Q("safety.check"), photo);
      QVERIFY2(kinds(r.value(Q("violations")).toArray()).contains(Q("photosensitive_red_flash")), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
    }
    { // 9 % of the frame flashing: under the area threshold
      fresh(320, 180);
      flasher(Q("#ffffff"), 3, 10, 0.3);
      const QJsonObject r = ok(Q("safety.check"), photo);
      QVERIFY2(r.value(Q("violations")).toArray().isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
    }
    const QJsonObject legib{{Q("checks"), QJsonArray{Q("text_legibility")}}};
    { // grey on grey fails, white with a stroke on black passes
      fresh(320, 180);
      ok(Q("shape.add"), {{Q("shape"), Q("rect")}, {Q("fill"), Q("#808080")}, {Q("width"), 320}, {Q("height"), 180}, {Q("startFrame"), 0}, {Q("durationFrames"), 90}});
      ok(Q("text.add"), {{Q("text"), Q("grey on grey")}, {Q("startFrame"), 1}, {Q("durationFrames"), 60}, {Q("style"), QJsonObject{{Q("fontSize"), 40}, {Q("color"), Q("#8a8a8a")}}}});
      const QJsonObject bad = ok(Q("safety.check"), legib);
      QVERIFY2(kinds(bad.value(Q("violations")).toArray()).contains(Q("text_contrast")), qPrintable(QString::fromUtf8(QJsonDocument(bad).toJson())));
      const QJsonObject m = bad.value(Q("violations")).toArray().at(0).toObject().value(Q("measured")).toObject();
      QVERIFY(m.value(Q("ratio")).toDouble() < 1.5);
      QVERIFY(bad.value(Q("violations")).toArray().at(0).toObject().value(Q("fix")).toObject().contains(Q("options")));
      fresh(320, 180);
      ok(Q("text.add"), {{Q("text"), Q("white on black")}, {Q("startFrame"), 0}, {Q("durationFrames"), 60},
                         {Q("style"), QJsonObject{{Q("fontSize"), 40}, {Q("color"), Q("#ffffff")}, {Q("strokeColor"), Q("#000000")}, {Q("strokeWidth"), 3}}}});
      const QJsonObject good = ok(Q("safety.check"), legib);
      QVERIFY2(good.value(Q("violations")).toArray().isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(good).toJson())));
      QVERIFY(good.value(Q("warnings")).toArray().isEmpty());
    }
    { // too short for six words
      fresh(320, 180);
      ok(Q("text.add"), {{Q("text"), Q("one two three four five six")}, {Q("startFrame"), 0}, {Q("durationFrames"), 20}, {Q("style"), QJsonObject{{Q("fontSize"), 30}, {Q("color"), Q("#ffffff")}}}});
      const QJsonObject r = ok(Q("safety.check"), legib);
      QVERIFY2(kinds(r.value(Q("warnings")).toArray()).contains(Q("text_duration")), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
    }
    { // TikTok: the right column is taken, the centre is free (9:16 defaults to tiktok)
      fresh(360, 640);
      ok(Q("text.add"), {{Q("text"), Q("Hello there")}, {Q("startFrame"), 0}, {Q("durationFrames"), 90}, {Q("x"), 150}, {Q("y"), 0}, {Q("style"), QJsonObject{{Q("fontSize"), 30}}}});
      const QJsonObject zone{{Q("checks"), QJsonArray{Q("safe_zones")}}};
      const QJsonObject r = ok(Q("safety.check"), zone);
      QCOMPARE(r.value(Q("platform")).toString(), Q("tiktok"));
      QVERIFY2(kinds(r.value(Q("violations")).toArray()).contains(Q("safe_zone_overlap")), qPrintable(QString::fromUtf8(QJsonDocument(r).toJson())));
      fresh(360, 640);
      ok(Q("text.add"), {{Q("text"), Q("Hello")}, {Q("startFrame"), 0}, {Q("durationFrames"), 90}, {Q("x"), 0}, {Q("y"), -100}, {Q("style"), QJsonObject{{Q("fontSize"), 30}}}});
      const QJsonObject clean = ok(Q("safety.check"), zone);
      QVERIFY2(clean.value(Q("violations")).toArray().isEmpty(), qPrintable(QString::fromUtf8(QJsonDocument(clean).toJson())));
      QCOMPARE(raw(Q("safety.check"), {{Q("platform"), Q("myspace")}}).error.code, Q("invalid_params"));
    }
  }

  void comfyCommands() {
    const QString fixturesDir = QStringLiteral(SF_COMFY_FIXTURES);
    const QString objectInfoPath = fixturesDir + QStringLiteral("/object_info_min.json");
    const QString txt2imgPath = fixturesDir + QStringLiteral("/txt2img_api.json");
    qputenv("SF_COMFY_OBJECT_INFO", objectInfoPath.toUtf8());

    // comfy.nodes {search:"sampler"} returns KSampler.
    const QJsonObject nodesRes = ok(Q("comfy.nodes"), {{Q("search"), Q("sampler")}});
    const QJsonArray nodes = nodesRes.value(Q("nodes")).toArray();
    QVERIFY(!nodes.isEmpty());
    bool foundKSampler = false;
    for (const auto& n : nodes) {
      if (n.toObject().value(Q("classType")).toString() == Q("KSampler")) foundKSampler = true;
    }
    QVERIFY(foundKSampler);

    // comfy.node.schema {classType:"KSampler"} has toolSchema.
    const QJsonObject schemaRes = ok(Q("comfy.node.schema"), {{Q("classType"), Q("KSampler")}});
    QCOMPARE(schemaRes.value(Q("classType")).toString(), Q("KSampler"));
    QVERIFY(schemaRes.contains(Q("toolSchema")));
    QVERIFY(schemaRes.value(Q("toolSchema")).toObject().contains(Q("properties")));

    // comfy.graph.load {path: txt2img}; validate gives valid.
    const QJsonObject loadRes = ok(Q("comfy.graph.load"), {{Q("path"), txt2imgPath}});
    const QString gid = loadRes.value(Q("graphId")).toString();
    QVERIFY(!gid.isEmpty());
    QCOMPARE(loadRes.value(Q("nodeCount")).toInt(), 7);

    const QJsonObject valRes = ok(Q("comfy.graph.validate"), {{Q("graphId"), gid}});
    QVERIFY(valRes.value(Q("valid")).toBool());
    QVERIFY(valRes.value(Q("issues")).toArray().isEmpty());

    // comfy.graph.op with set 3.sampler_name "eulr" succeeds, and its issues has one bad_enum.
    const QJsonObject opRes = ok(Q("comfy.graph.op"), {
      {Q("graphId"), gid},
      {Q("ops"), QJsonArray{
        QJsonObject{{Q("op"), Q("set")}, {Q("id"), Q("3")}, {Q("input"), Q("sampler_name")}, {Q("value"), Q("eulr")}}
      }}
    });
    QCOMPARE(opRes.value(Q("graphId")).toString(), gid);
    const QJsonArray opIssues = opRes.value(Q("issues")).toArray();
    QCOMPARE(opIssues.size(), 1);
    QCOMPARE(opIssues.at(0).toObject().value(Q("code")).toString(), Q("bad_enum"));

    // comfy.graph.undo makes it valid again.
    const QJsonObject undoRes = ok(Q("comfy.graph.undo"), {{Q("graphId"), gid}});
    QVERIFY(undoRes.value(Q("undone")).toBool());

    const QJsonObject postUndoVal = ok(Q("comfy.graph.validate"), {{Q("graphId"), gid}});
    QVERIFY(postUndoVal.value(Q("valid")).toBool());
    QVERIFY(postUndoVal.value(Q("issues")).toArray().isEmpty());

    // comfy.graph.op with a bad op returns an error, and the node count is unchanged.
    const int countBefore = ok(Q("comfy.graph.get"), {{Q("graphId"), gid}}).value(Q("nodeCount")).toInt();
    const Result badOpRes = raw(Q("comfy.graph.op"), {
      {Q("graphId"), gid},
      {Q("ops"), QJsonArray{
        QJsonObject{{Q("op"), Q("connect")}, {Q("from"), Q("999")}, {Q("output"), 0}, {Q("to"), Q("3")}, {Q("input"), Q("model")}}
      }}
    });
    QVERIFY(!badOpRes.ok);
    const int countAfter = ok(Q("comfy.graph.get"), {{Q("graphId"), gid}}).value(Q("nodeCount")).toInt();
    QCOMPARE(countAfter, countBefore);

    // comfy.status never fails.
    const Result statusRes = raw(Q("comfy.status"));
    QVERIFY(statusRes.ok);
    QVERIFY(statusRes.value.toObject().contains(Q("reachable")));
  }

  void variationsSheet() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{clip_}}});
    const QString video = firstAsset(Q("video"));
    ok(Q("clip.add"), {{Q("assetId"), video}});
    const QJsonArray clipItems = items();
    QVERIFY(!clipItems.isEmpty());
    const QString clipId = clipItems.at(0).toObject().value(Q("id")).toString();

    // Probe offscreen renderer availability
    const Result probe = raw(Q("frame.render"), {{Q("frame"), 0}});
    if (!probe.ok && probe.error.code == Q("unavailable") && probe.error.message.contains(Q("renderer"))) {
      QSKIP("no D3D11 device for the offscreen renderer");
    }

    // 3. Capture timeline and history before variations.sheet
    const QJsonObject tlBefore = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    const QJsonObject histBefore = ok(Q("history.list"));

    // Find look name from style.list or lookNames()
    const QJsonObject styleList = ok(Q("style.list"));
    QString lookName = QStringLiteral("teal-orange");
    const QJsonArray looksList = styleList.value(Q("looks")).toObject().value(Q("items")).toArray();
    if (!looksList.isEmpty()) {
      bool foundTeal = false;
      for (const auto& l : looksList) {
        if (l.toObject().value(Q("name")).toString() == Q("teal-orange")) {
          foundTeal = true;
          break;
        }
      }
      if (!foundTeal) lookName = looksList.at(0).toObject().value(Q("name")).toString();
    }

    const QString outPng = dir_.filePath(Q("test_sheet.png"));

    // 2. Call variations.sheet with 3 variants: look, effect "invert", and raw ops []
    const Result rSheet = raw(Q("variations.sheet"), {
      {Q("itemId"), clipId},
      {Q("path"), outPng},
      {Q("variants"), QJsonArray{
        QJsonObject{{Q("label"), Q("my-look")}, {Q("look"), QJsonObject{{Q("look"), lookName}, {Q("intensity"), 1.0}}}},
        QJsonObject{{Q("label"), Q("my-invert")}, {Q("effect"), QJsonObject{{Q("effectId"), Q("invert")}}}},
        QJsonObject{{Q("label"), Q("my-raw")}, {Q("ops"), QJsonArray{}}}
      }}
    });
    QVERIFY2(rSheet.ok, qPrintable(rSheet.error.message));
    const QJsonObject res = rSheet.value.toObject();

    // Check cells has length 4 (original + 3) and letters A..D
    const QJsonArray cells = res.value(Q("cells")).toArray();
    QCOMPARE(cells.size(), 4);
    QCOMPARE(cells.at(0).toObject().value(Q("letter")).toString(), Q("A"));
    QCOMPARE(cells.at(1).toObject().value(Q("letter")).toString(), Q("B"));
    QCOMPARE(cells.at(2).toObject().value(Q("letter")).toString(), Q("C"));
    QCOMPARE(cells.at(3).toObject().value(Q("letter")).toString(), Q("D"));
    QCOMPARE(cells.at(0).toObject().value(Q("label")).toString(), Q("original"));

    // Check that PNG exists and its size matches width/height
    QVERIFY(QFile::exists(outPng));
    QImage loadedSheet(outPng);
    QCOMPARE(loadedSheet.width(), res.value(Q("width")).toInt());
    QCOMPARE(loadedSheet.height(), res.value(Q("height")).toInt());

    // Check measurements:
    const double origLuma = cells.at(0).toObject().value(Q("measured")).toObject().value(Q("meanLuma")).toDouble();
    const double invLuma = cells.at(2).toObject().value(Q("measured")).toObject().value(Q("meanLuma")).toDouble();
    const double rawLuma = cells.at(3).toObject().value(Q("measured")).toObject().value(Q("meanLuma")).toDouble();

    // The invert cell's meanLuma is about 1 - original (±0.08)
    QVERIFY2(std::fabs(invLuma - (1.0 - origLuma)) <= 0.08,
             qPrintable(QStringLiteral("invLuma: %1 vs expected ~ %2").arg(invLuma).arg(1.0 - origLuma)));

    // The raw-ops [] cell's measurement equals the original's (±0.01)
    QVERIFY2(std::fabs(rawLuma - origLuma) <= 0.01,
             qPrintable(QStringLiteral("rawLuma: %1 vs origLuma: %2").arg(rawLuma).arg(origLuma)));

    // 3. Project history length is unchanged and timeline.get is byte-identical
    const QJsonObject tlAfter = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    const QJsonObject histAfter = ok(Q("history.list"));
    QCOMPARE(histAfter.value(Q("total")).toInt(), histBefore.value(Q("total")).toInt());
    QCOMPARE(QJsonDocument(tlAfter).toJson(), QJsonDocument(tlBefore).toJson());

    // 4. variations.looks returns as many cells as there are looks, plus 1
    const int lookCount = looksList.size();
    const Result rLooks = raw(Q("variations.looks"), {{Q("itemId"), clipId}});
    QVERIFY2(rLooks.ok, qPrintable(rLooks.error.message));
    const QJsonArray lookCells = rLooks.value.toObject().value(Q("cells")).toArray();
    QCOMPARE(lookCells.size(), lookCount + 1);

    // 5. An invalid look name in one variant puts that variant in errors[] with a grey cell. The call itself still succeeds.
    const Result rBad = raw(Q("variations.sheet"), {
      {Q("itemId"), clipId},
      {Q("variants"), QJsonArray{
        QJsonObject{{Q("label"), Q("bad-look")}, {Q("look"), QJsonObject{{Q("look"), Q("nonexistent_look_name_12345")}}}}
      }}
    });
    QVERIFY(rBad.ok);
    const QJsonObject badRes = rBad.value.toObject();
    const QJsonArray badErrors = badRes.value(Q("errors")).toArray();
    QCOMPARE(badErrors.size(), 1);
    QCOMPARE(badRes.value(Q("cells")).toArray().size(), 2); // original + bad-look
  }

  void assembleCommand() {
    ok(Q("media.import"), {{Q("paths"), QJsonArray{story_, talk_, beats_}}});
    const QJsonArray assets = ok(Q("assets.list")).value(Q("assets")).toArray();
    QString storyId, talkId, beatsId;
    for (const auto& aVal : assets) {
      const QJsonObject aObj = aVal.toObject();
      const QString path = aObj.value(Q("path")).toString();
      if (path.contains(Q("story"))) storyId = aObj.value(Q("id")).toString();
      else if (path.contains(Q("talk"))) talkId = aObj.value(Q("id")).toString();
      else if (path.contains(Q("beats"))) beatsId = aObj.value(Q("id")).toString();
    }
    QVERIFY(!storyId.isEmpty());
    QVERIFY(!talkId.isEmpty());
    QVERIFY(!beatsId.isEmpty());

    QJsonObject rMusic = ok(Q("audio.add"), {{Q("assetId"), beatsId}});
    const QString musicItemId = rMusic.value(Q("items")).toArray().first().toObject().value(Q("id")).toString();
    QVERIFY(!musicItemId.isEmpty());

    Result rDry = raw(Q("edit.assemble"), {
      {Q("assetIds"), QJsonArray{storyId, talkId}},
      {Q("musicItemId"), musicItemId},
      {Q("pacing"), Q("social-short")},
      {Q("apply"), false}
    });
    QVERIFY2(rDry.ok, qPrintable(rDry.error.message));
    const QJsonObject dryVal = rDry.value.toObject();
    QCOMPARE(dryVal.value(Q("applied")).toBool(), false);
    const QJsonObject plan = dryVal.value(Q("plan")).toObject();
    const QJsonArray shots = plan.value(Q("shots")).toArray();
    QVERIFY(!shots.isEmpty());
    const int shotCount = shots.size();

    Result rApply = raw(Q("edit.assemble"), {
      {Q("assetIds"), QJsonArray{storyId, talkId}},
      {Q("musicItemId"), musicItemId},
      {Q("pacing"), Q("social-short")},
      {Q("apply"), true}
    });
    QVERIFY2(rApply.ok, qPrintable(rApply.error.message));
    const QJsonObject applyVal = rApply.value.toObject();
    QCOMPARE(applyVal.value(Q("applied")).toBool(), true);
    QVERIFY(applyVal.contains(Q("check")));

    QJsonObject tlAfter = ok(Q("timeline.get"), {{Q("mode"), Q("summary")}});
    int videoItemCount = 0;
    for (const auto& tVal : tlAfter.value(Q("tracks")).toArray()) {
      const QJsonObject tObj = tVal.toObject();
      if (tObj.value(Q("kind")).toString() == Q("video")) {
        videoItemCount += tObj.value(Q("items")).toArray().size();
      }
    }
    QCOMPARE(videoItemCount, shotCount);

    Result rUndo = raw(Q("history.undo"), {});
    QVERIFY(rUndo.ok);
    QJsonObject tlRestored = ok(Q("timeline.get"), {{Q("mode"), Q("summary")}});
    int videoItemCountRestored = 0;
    for (const auto& tVal : tlRestored.value(Q("tracks")).toArray()) {
      const QJsonObject tObj = tVal.toObject();
      if (tObj.value(Q("kind")).toString() == Q("video")) {
        videoItemCountRestored += tObj.value(Q("items")).toArray().size();
      }
    }
    QCOMPARE(videoItemCountRestored, 0);
  }

  void unknownTextureRejected() {
    const QString item = ok(Q("text.add"), {{Q("text"), Q("T")}, {Q("startFrame"), 0}, {Q("durationFrames"), 20}}).value(Q("created")).toObject().value(Q("items")).toArray().at(0).toString();
    QVERIFY(!item.isEmpty());
    ok(Q("item.update"), {{Q("itemId"), item}, {Q("patch"), QJsonObject{{Q("fx"), QJsonArray{QJsonObject{{Q("id"), Q("fx1")}, {Q("effectId"), Q("gl_luma")}}}}}}});
    const Result r = raw(Q("effect.set"), {{Q("itemId"), item}, {Q("fxId"), Q("fx1")}, {Q("params"), QJsonObject{{Q("luma"), Q("bogus_texture_name")}}}});
    QCOMPARE(r.error.code, Q("invalid_params"));
    QVERIFY(r.error.message.contains(Q("bogus_texture_name")) || r.error.message.contains(Q("expects one of")));
  }
};

QTEST_MAIN(TstEngine)
#include "tst_engine.moc"
