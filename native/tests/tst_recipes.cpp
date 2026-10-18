// Tests for recipes (library data, placeholder expansion, single undo, rollback) and reference.analyze.

#include "core/schema.h"
#include "engine/engine.h"
#include "engine/style_library.h"
#include "render/effects.h"
#include "media_testutil.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <cmath>
#include <memory>
#include <vector>

using namespace sf;
using namespace sf::engine;
using namespace sf::engine::style;
using namespace sf::test;

namespace {

QString Q(const char* s) { return QString::fromLatin1(s); }

bool writeClickWav(const QString& path, double bpm, double seconds, double first = 0.13) {
  const int sr = 48000;
  const size_t n = static_cast<size_t>(seconds * sr);
  std::vector<qint16> pcm(n * 2, 0);
  for (double t0 = first; t0 < seconds - 0.1; t0 += 60.0 / bpm) {
    for (int i = 0; i < 2400; ++i) {
      const size_t k = static_cast<size_t>(t0 * sr) + static_cast<size_t>(i);
      if (k >= n) break;
      const double t = i / static_cast<double>(sr);
      const qint16 x = static_cast<qint16>(16000.0 * std::exp(-t / 0.008) * std::sin(6.283185307179586 * 3000.0 * t));
      pcm[k * 2] = pcm[k * 2 + 1] = x;
    }
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

} // namespace

class TstRecipes : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString clipPath_, audioPath_;
  std::unique_ptr<Engine> engine_;

  QJsonObject ok(const QString& cmd, const QJsonObject& args = {}) {
    const Result r = engine_->call(cmd, args);
    if (!r.ok) qWarning().noquote() << cmd << "failed:" << r.error.code << r.error.message;
    return r.ok ? r.value.toObject() : QJsonObject{{Q("__failed"), true}};
  }

  Result raw(const QString& cmd, const QJsonObject& args = {}) {
    return engine_->call(cmd, args);
  }

  void setupProjectWithMedia() {
    engine_ = std::make_unique<Engine>();
    ok(Q("project.new"), {{Q("name"), Q("TestRecipeProject")}, {Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}});

    const QJsonObject vImp = ok(Q("media.import"), {{Q("paths"), QJsonArray{clipPath_}}});
    const QString vAssetId = vImp.value(Q("added")).toArray().first().toString();
    const QJsonObject aImp = ok(Q("media.import"), {{Q("paths"), QJsonArray{audioPath_}}});
    const QString aAssetId = aImp.value(Q("added")).toArray().first().toString();

    // 2 video clips on main video track, 1 audio clip on main audio track
    ok(Q("clip.add"), {{Q("assetId"), vAssetId}, {Q("startFrame"), 0}, {Q("durationFrames"), 50}});
    ok(Q("clip.add"), {{Q("assetId"), vAssetId}, {Q("startFrame"), 50}, {Q("durationFrames"), 50}});
    ok(Q("audio.add"), {{Q("assetId"), aAssetId}, {Q("startFrame"), 0}, {Q("durationFrames"), 100}});
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");

    clipPath_ = dir_.filePath(QStringLiteral("test_clip.mp4"));
    audioPath_ = dir_.filePath(QStringLiteral("test_audio.wav"));

    QString log;
    QVERIFY2(runFfmpeg({
      Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=blue:d=4:r=25:s=320x180"),
      Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"),
      Q("-y"), clipPath_
    }, &log), qPrintable(log));

    QVERIFY(writeClickWav(audioPath_, 120.0, 5.0));
  }

  void cleanup() {
    engine_.reset();
    qunsetenv("SF_RECIPES_PATH");
  }

  // 1. recipesReferenceRealNames: every look, mix, pacing, effect and transition name used in recipes.json exists.
  void recipesReferenceRealNames() {
    const auto& list = recipes();
    QCOMPARE(list.size(), size_t{6});

    const QStringList expectedNames = {
      Q("punchy-promo"), Q("calm-documentary"), Q("music-video"),
      Q("vlog-clean"), Q("trailer"), Q("retro-vhs")
    };
    for (const QString& expected : expectedNames) {
      const Recipe* r = findRecipe(expected);
      QVERIFY2(r != nullptr, qPrintable(QStringLiteral("Missing recipe: %1").arg(expected)));
      QVERIFY(!r->description.isEmpty());
      QVERIFY(!r->whenToUse.isEmpty());
      QVERIFY(!r->steps.empty());

      for (const RecipeStep& s : r->steps) {
        if (s.command == Q("look.apply")) {
          const QString lk = s.args.value(Q("look")).toString();
          QVERIFY2(findLook(lk) != nullptr, qPrintable(QStringLiteral("Recipe %1: unknown look %2").arg(expected, lk)));
        } else if (s.command == Q("mix.applyProfile")) {
          const QString mx = s.args.value(Q("profile")).toString();
          QVERIFY2(findMix(mx) != nullptr, qPrintable(QStringLiteral("Recipe %1: unknown mix %2").arg(expected, mx)));
        } else if (s.command == Q("effect.add")) {
          const QString ef = s.args.value(Q("effectId")).toString();
          QVERIFY2(render::FxRegistry::instance().find(ef) != nullptr, qPrintable(QStringLiteral("Recipe %1: unknown effect %2").arg(expected, ef)));
        } else if (s.command == Q("transition.add")) {
          const QString tr = s.args.value(Q("effectId")).toString();
          QVERIFY2(render::FxRegistry::instance().find(tr) != nullptr, qPrintable(QStringLiteral("Recipe %1: unknown transition %2").arg(expected, tr)));
        }
      }

      for (const RecipeStep& v : r->verify) {
        if (v.command == Q("pacing.check")) {
          const QString pc = v.args.value(Q("profile")).toString();
          QVERIFY2(findPacing(pc) != nullptr, qPrintable(QStringLiteral("Recipe %1 verify: unknown pacing %2").arg(expected, pc)));
        }
      }
    }

    const QStringList libErrors = validateLibrary();
    QVERIFY2(libErrors.isEmpty(), qPrintable(libErrors.join(Q("\n"))));
  }

  // 2. placeholderResolution: on a project with 2 video clips and 1 audio clip, recipe.apply {name: "vlog-clean", dryRun: true}:
  //    - $eachVideoItem expands into 2 steps;
  //    - $videoItems holds 2 ids.
  void placeholderResolution() {
    setupProjectWithMedia();

    const QJsonObject res = ok(Q("recipe.apply"), {{Q("name"), Q("vlog-clean")}, {Q("dryRun"), true}});
    QVERIFY(res.value(Q("dryRun")).toBool());
    const QJsonArray steps = res.value(Q("steps")).toArray();

    // Check $videoItems in look.apply holds 2 ids
    bool lookFound = false;
    int effectStepCount = 0;
    for (const QJsonValue& v : steps) {
      const QJsonObject s = v.toObject();
      const QString cmd = s.value(Q("command")).toString();
      if (cmd == Q("look.apply")) {
        const QJsonArray itemIds = s.value(Q("args")).toObject().value(Q("itemIds")).toArray();
        QCOMPARE(itemIds.size(), 2);
        lookFound = true;
      } else if (cmd == Q("effect.add")) {
        ++effectStepCount;
        const QString itemId = s.value(Q("args")).toObject().value(Q("itemId")).toString();
        QVERIFY(!itemId.isEmpty());
        QVERIFY(itemId != Q("$eachVideoItem"));
      }
    }

    QVERIFY(lookFound);
    // In vlog-clean, effect.add has itemId: "$eachVideoItem", so it should expand to 2 steps
    QCOMPARE(effectStepCount, 2);
  }

  // 3. applyIsOneUndo: apply punchy-promo. History grows by exactly 1. One undo makes timeline.get equal to the state before.
  void applyIsOneUndo() {
    setupProjectWithMedia();

    const QJsonObject beforeState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});

    const QJsonObject res = ok(Q("recipe.apply"), {{Q("name"), Q("punchy-promo")}});
    QVERIFY(!res.value(Q("__failed")).toBool());
    QCOMPARE(res.value(Q("undoSteps")).toInt(), 1);

    const QJsonObject afterState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    QVERIFY(beforeState != afterState);

    // Call history.undo via engine undo command
    const Result u = engine_->call(Q("history.undo"));
    QVERIFY(u.ok);

    const QJsonObject restoredState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    QCOMPARE(restoredState, beforeState);
  }

  // 4. failingStepRollsBack: register a recipe from a temp JSON with a bogus step {"command": "look.apply", "args": {"look": "nope"}} as step 2.
  //    If loading test recipes from a path needs an env override, add SF_RECIPES_PATH to the loader. Check:
  //    - the result is a failure naming step index 1;
  //    - the timeline is unchanged;
  //    - history is unchanged.
  void failingStepRollsBack() {
    setupProjectWithMedia();

    const QJsonObject beforeState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});

    QTemporaryFile tempFile(dir_.filePath(QStringLiteral("temp_recipes_XXXXXX.json")));
    QVERIFY(tempFile.open());
    const QJsonObject tempDoc{
      {Q("version"), 1},
      {Q("kind"), Q("recipe")},
      {Q("items"), QJsonArray{
        QJsonObject{
          {Q("name"), Q("test-fail")},
          {Q("description"), Q("Failing recipe test")},
          {Q("whenToUse"), QJsonArray{Q("test")}},
          {Q("avoidWhen"), QJsonArray{}},
          {Q("steps"), QJsonArray{
            QJsonObject{{Q("command"), Q("look.apply")}, {Q("args"), QJsonObject{{Q("look"), Q("neutral-clean")}, {Q("itemIds"), Q("$videoItems")}, {Q("apply"), true}, {Q("force"), true}}}},
            QJsonObject{{Q("command"), Q("look.apply")}, {Q("args"), QJsonObject{{Q("look"), Q("nope")}, {Q("itemIds"), Q("$videoItems")}, {Q("apply"), true}}}}
          }},
          {Q("verify"), QJsonArray{}}
        }
      }}
    };
    tempFile.write(QJsonDocument(tempDoc).toJson());
    tempFile.close();

    qputenv("SF_RECIPES_PATH", tempFile.fileName().toUtf8());

    const Result r = raw(Q("recipe.apply"), {{Q("name"), Q("test-fail")}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("recipe_failed"));
    const QJsonObject details = r.error.details.toObject();
    QCOMPARE(details.value(Q("stepIndex")).toInt(), 1);
    QCOMPARE(details.value(Q("command")).toString(), Q("look.apply"));
    QVERIFY2(r.error.message.contains(Q("Step 1 (look.apply) failed")), qPrintable(r.error.message));

    const QJsonObject afterState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    QCOMPARE(afterState, beforeState);

    // Verify history did not grow: an undo would fail or undo the setup clip.add
    const Result undoResult = engine_->call(Q("history.undo"));
    QVERIFY(undoResult.ok);
    // After that undo, the state is DIFFERENT from beforeState (it undid the last clip.add)
    const QJsonObject stateAfterUndo = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    QVERIFY(stateAfterUndo != beforeState);
  }

  // 4b. recipe failure inside an outer group rolls back partial steps and restores outer depth
  void testFailRollsBackInNestedGroup() {
    setupProjectWithMedia();
    const QJsonObject beforeState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});

    auto* sess = engine_->session();
    QVERIFY(sess && sess->project);
    sess->project->beginGroup(QStringLiteral("OuterGroup"));
    QCOMPARE(sess->project->groupDepth(), 1);

    QTemporaryFile tempFile(dir_.filePath(QStringLiteral("temp_recipes_nested_XXXXXX.json")));
    QVERIFY(tempFile.open());
    const QJsonObject tempDoc{
      {Q("version"), 1},
      {Q("kind"), Q("recipe")},
      {Q("items"), QJsonArray{
        QJsonObject{
          {Q("name"), Q("nested-fail")},
          {Q("description"), Q("Fails at step 1")},
          {Q("whenToUse"), QJsonArray{Q("test")}},
          {Q("avoidWhen"), QJsonArray{}},
          {Q("steps"), QJsonArray{
            QJsonObject{{Q("command"), Q("look.apply")}, {Q("args"), QJsonObject{{Q("look"), Q("neutral-clean")}, {Q("itemIds"), Q("$videoItems")}, {Q("apply"), true}, {Q("force"), true}}}},
            QJsonObject{{Q("command"), Q("look.apply")}, {Q("args"), QJsonObject{{Q("look"), Q("nope")}, {Q("itemIds"), Q("$videoItems")}, {Q("apply"), true}}}}
          }},
          {Q("verify"), QJsonArray{}}
        }
      }}
    };
    tempFile.write(QJsonDocument(tempDoc).toJson());
    tempFile.close();

    qputenv("SF_RECIPES_PATH", tempFile.fileName().toUtf8());

    const Result r = raw(Q("recipe.apply"), {{Q("name"), Q("nested-fail")}});
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("recipe_failed"));

    // Depth must have returned to 1 (outer group remains open)
    QCOMPARE(sess->project->groupDepth(), 1);

    // State must have been rolled back to beforeState (partial step 0 reverted)
    const QJsonObject afterState = ok(Q("timeline.get"), {{Q("mode"), Q("detail")}});
    QCOMPARE(afterState, beforeState);

    // Ending the outer group commits cleanly with zero leftover recipe edits
    sess->project->endGroup();
    QCOMPARE(sess->project->groupDepth(), 0);
  }

  // 5. referenceAnalyze:
  //    - Generate a 12 s test video with SF_FFMPEG_EXE: 4 solid-colour segments of 3 s each with a 120 BPM click track.
  //    - Run reference.analyze and check:
  //      - 4 shots (±1);
  //      - aslSec ≈ 3 (±0.6);
  //      - bpm ≈ 120 (±2);
  //      - styleSpec present;
  //      - nearest.pacing non-empty and sorted ascending.
  void referenceAnalyze() {
    engine_ = std::make_unique<Engine>();

    const QString clickPath = dir_.filePath(QStringLiteral("click120.wav"));
    QVERIFY(writeClickWav(clickPath, 120.0, 12.0, 0.0));

    const QString refVideoPath = dir_.filePath(QStringLiteral("ref_video_12s.mp4"));
    QString log;
    QVERIFY2(runFfmpeg({
      Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=red:d=3:r=25:s=320x180"),
      Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=blue:d=3:r=25:s=320x180"),
      Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=yellow:d=3:r=25:s=320x180"),
      Q("-f"), Q("lavfi"), Q("-i"), Q("color=c=green:d=3:r=25:s=320x180"),
      Q("-i"), clickPath,
      Q("-filter_complex"), Q("[0:v][1:v][2:v][3:v]concat=n=4:v=1:a=0[v]"),
      Q("-map"), Q("[v]"), Q("-map"), Q("4:a"),
      Q("-c:v"), Q("libx264"), Q("-preset"), Q("ultrafast"), Q("-pix_fmt"), Q("yuv420p"),
      Q("-c:a"), Q("aac"), Q("-shortest"), Q("-y"), refVideoPath
    }, &log), qPrintable(log));

    const Result res = raw(Q("reference.analyze"), {{Q("path"), refVideoPath}});
    QVERIFY2(res.ok, qPrintable(res.error.message));

    const QJsonObject out = res.value.toObject();
    const QJsonObject cuts = out.value(Q("cuts")).toObject();
    const int shots = cuts.value(Q("shots")).toInt();
    const double aslSec = cuts.value(Q("aslSec")).toDouble();

    // 4 shots (±1)
    QVERIFY2(shots >= 3 && shots <= 5, qPrintable(QString::number(shots)));
    // aslSec ≈ 3 (±0.6)
    QVERIFY2(aslSec >= 2.4 && aslSec <= 3.6, qPrintable(QString::number(aslSec)));

    const QJsonObject audio = out.value(Q("audio")).toObject();
    const double bpm = audio.value(Q("bpm")).toDouble();
    // bpm ≈ 120 (±2)
    QVERIFY2(bpm >= 118.0 && bpm <= 122.0, qPrintable(QString::number(bpm)));

    // styleSpec present
    QVERIFY(out.contains(Q("styleSpec")));
    QVERIFY(out.value(Q("styleSpec")).isObject());

    // nearest.pacing non-empty and sorted ascending
    const QJsonObject nearest = out.value(Q("nearest")).toObject();
    const QJsonArray pacing = nearest.value(Q("pacing")).toArray();
    QVERIFY(!pacing.isEmpty());

    double prevDist = -1.0;
    for (const QJsonValue& pv : pacing) {
      const double d = pv.toObject().value(Q("distance")).toDouble();
      QVERIFY(d >= prevDist);
      prevDist = d;
    }
  }
};

QTEST_MAIN(TstRecipes)
#include "tst_recipes.moc"
