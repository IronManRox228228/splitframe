#include "copilot/lint_engine.h"
#include "core/ops.h"
#include "core/time.h"

#include <QTest>

using namespace sf;
using namespace sf::copilot;

class TstCopilotLint : public QObject {
  Q_OBJECT

private:
  TimelineDoc createDocWithTracks() {
    TimelineDoc doc;
    doc.project.id = QStringLiteral("prj_test");
    doc.project.name = QStringLiteral("Lint Test");
    doc.project.fps = 30;

    Track v1{.id = QStringLiteral("trk_v1"), .kind = TrackKind::Video, .name = QStringLiteral("V1")};
    Track v2{.id = QStringLiteral("trk_v2"), .kind = TrackKind::Video, .name = QStringLiteral("V2")};
    Track a1{.id = QStringLiteral("trk_a1"), .kind = TrackKind::Audio, .name = QStringLiteral("A1")};
    doc.tracks = {v1, v2, a1};
    return doc;
  }

private slots:
  void testFlashFrame() {
    TimelineDoc doc = createDocWithTracks();
    Item flash;
    flash.id = QStringLiteral("itm_flash");
    flash.trackId = QStringLiteral("trk_v1");
    flash.startFrame = 30;
    flash.durationFrames = 2; // only 2 frames at 30fps = 66ms (< 100ms)
    flash.props = VideoProps{};
    doc.items.push_back(flash);

    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("flash_frame"));
    QCOMPARE(issues[0].severity, LintSeverity::Warning);
    QCOMPARE(issues[0].itemId, QStringLiteral("itm_flash"));
    QVERIFY(issues[0].autoFixAvailable);
    QCOMPARE(issues[0].fixOps.size(), 1);
    QCOMPARE(issues[0].fixOps[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.remove"));
  }

  void testUnintentionalGap() {
    TimelineDoc doc = createDocWithTracks();
    Item it1;
    it1.id = QStringLiteral("itm_1");
    it1.trackId = QStringLiteral("trk_v1");
    it1.startFrame = 0;
    it1.durationFrames = 60; // 0..60
    it1.props = VideoProps{};

    Item it2;
    it2.id = QStringLiteral("itm_2");
    it2.trackId = QStringLiteral("trk_v1");
    it2.startFrame = 65; // gap of 5 frames = 166ms (<= 250ms default threshold)
    it2.durationFrames = 60;
    it2.props = VideoProps{};

    doc.items = {it1, it2};

    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("unintentional_gap"));
    QCOMPARE(issues[0].severity, LintSeverity::Info); // Info severity per review recommendation
    QCOMPARE(issues[0].startMs, 2000); // 60 frames * 1000 / 30 = 2000ms
    QVERIFY(issues[0].autoFixAvailable);

    // Verify intentional large gap (> 250ms) is NOT flagged
    doc.items[1].startFrame = 90; // gap = 30 frames = 1000ms
    const auto noIssues = engine.analyze(doc, {});
    QCOMPARE(noIssues.size(), 0);
  }

  void testMidWordCut() {
    TimelineDoc doc = createDocWithTracks();
    Item it;
    it.id = QStringLiteral("itm_speech");
    it.trackId = QStringLiteral("trk_v1");
    it.assetId = QStringLiteral("ast_interview");
    it.startFrame = 0;
    it.durationFrames = 90;
    it.sourceInFrame = 45; // 45 frames at 30fps = 1.5s
    it.props = VideoProps{};
    doc.items.push_back(it);

    // Mock speech span: active speech from 1.0s to 3.0s
    index::SpeechSpan sp;
    sp.id = QStringLiteral("sp_1");
    sp.assetId = QStringLiteral("ast_interview");
    sp.startSec = 1.0;
    sp.endSec = 3.0;
    sp.isSpeech = true;

    LintOptions opts;
    opts.speechSpans.push_back(sp);

    Asset ast;
    ast.id = QStringLiteral("ast_interview");
    ast.durationMs = 10000;

    LintEngine engine;
    const auto issues = engine.analyze(doc, {ast}, nullptr, opts);
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("mid_word_cut"));
    QCOMPARE(issues[0].severity, LintSeverity::Error);
    QVERIFY(issues[0].autoFixAvailable);
    QCOMPARE(issues[0].fixOps[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.trim"));
  }

  void testJumpCutSameAngle() {
    TimelineDoc doc = createDocWithTracks();
    Item take1;
    take1.id = QStringLiteral("itm_take1");
    take1.trackId = QStringLiteral("trk_v1");
    take1.assetId = QStringLiteral("ast_camA");
    take1.startFrame = 0;
    take1.durationFrames = 60; // 0..2.0s
    take1.sourceInFrame = 0;
    take1.props = VideoProps{};

    Item take2;
    take2.id = QStringLiteral("itm_take2");
    take2.trackId = QStringLiteral("trk_v1");
    take2.assetId = QStringLiteral("ast_camA"); // same asset
    take2.startFrame = 60;
    take2.durationFrames = 60;
    take2.sourceInFrame = 75; // source delta is only 15 frames = 0.5s (< 3.0s window)
    take2.props = VideoProps{};

    doc.items = {take1, take2};

    Asset camA;
    camA.id = QStringLiteral("ast_camA");
    camA.durationMs = 10000;
    std::vector<Asset> assets = {camA};

    LintEngine engine;
    const auto issues = engine.analyze(doc, assets);
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("jump_cut_same_angle"));
    QCOMPARE(issues[0].severity, LintSeverity::Warning);

    // Adding a cutaway clip on V2 covering the cut point resolves the jump cut
    Item broll;
    broll.id = QStringLiteral("itm_broll");
    broll.trackId = QStringLiteral("trk_v2");
    broll.startFrame = 50;
    broll.durationFrames = 30; // covers frame 60
    broll.props = VideoProps{};
    doc.items.push_back(broll);

    const auto resolved = engine.analyze(doc, assets);
    QCOMPARE(resolved.size(), 0);
  }

  void testAudioCompoundClipping() {
    TimelineDoc doc = createDocWithTracks();
    Item audio;
    audio.id = QStringLiteral("itm_loud");
    audio.trackId = QStringLiteral("trk_a1");
    audio.startFrame = 0;
    audio.durationFrames = 90;
    audio.volume = 1.5; // boosted (+3.5 dB)
    AudioProps ap;
    ap.fadeOutFrames = 2; // fade out present so abrupt end is not flagged
    audio.props = ap;
    doc.items.push_back(audio);

    // Mixer strip is unity, but item volume > 1.0 causes compound clipping
    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("audio_clipping"));
    QCOMPARE(issues[0].severity, LintSeverity::Error);
    QVERIFY(issues[0].autoFixAvailable);
    QCOMPARE(issues[0].fixOps[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.update"));
  }

  void testAudioAbruptEnd() {
    TimelineDoc doc = createDocWithTracks();
    Item audio;
    audio.id = QStringLiteral("itm_snd");
    audio.trackId = QStringLiteral("trk_a1");
    audio.startFrame = 0;
    audio.durationFrames = 90;
    audio.volume = 0.9;
    AudioProps ap;
    ap.fadeOutFrames = 0; // abrupt end
    audio.props = ap;
    doc.items.push_back(audio);

    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("audio_abrupt_end"));
    QCOMPARE(issues[0].severity, LintSeverity::Warning);
    QVERIFY(issues[0].autoFixAvailable);
  }

  void testCaptionSafeZone() {
    TimelineDoc doc = createDocWithTracks();
    Item cap;
    cap.id = QStringLiteral("itm_cap");
    cap.trackId = QStringLiteral("trk_v2");
    cap.startFrame = 0;
    cap.durationFrames = 60;
    CaptionProps cp;
    cp.style.placementY = 0.20; // in upper fifth (unsafe)
    cap.props = cp;
    doc.items.push_back(cap);

    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("caption_safe_zone"));
    QCOMPARE(issues[0].severity, LintSeverity::Warning);
    QVERIFY(issues[0].autoFixAvailable);
  }

  void testMissingMedia() {
    TimelineDoc doc = createDocWithTracks();
    Item it;
    it.id = QStringLiteral("itm_ref");
    it.trackId = QStringLiteral("trk_v1");
    it.assetId = QStringLiteral("ast_ghost"); // nonexistent asset
    it.startFrame = 0;
    it.durationFrames = 60;
    it.props = VideoProps{};
    doc.items.push_back(it);

    LintEngine engine;
    const auto issues = engine.analyze(doc, {});
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues[0].ruleId, QStringLiteral("missing_media"));
    QCOMPARE(issues[0].severity, LintSeverity::Error);
    QCOMPARE(issues[0].autoFixAvailable, false);
  }
};

QTEST_GUILESS_MAIN(TstCopilotLint)
#include "tst_copilot_lint.moc"
