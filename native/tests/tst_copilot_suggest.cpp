#include "copilot/suggestion_engine.h"
#include "editor/project.h"

#include <QSignalSpy>
#include <QTest>

using namespace sf;
using namespace sf::editor;
using namespace sf::copilot;

class TstCopilotSuggest : public QObject {
  Q_OBJECT

private:
  sf::editor::Project project_;

  TimelineDoc createDoc() {
    TimelineDoc doc;
    doc.project.fps = 30;

    Track v1{.id = QStringLiteral("trk_v1"), .kind = TrackKind::Video, .name = QStringLiteral("V1")};
    Track v2{.id = QStringLiteral("trk_v2"), .kind = TrackKind::Video, .name = QStringLiteral("V2")};
    doc.tracks = {v1, v2};

    Item it;
    it.id = QStringLiteral("itm_1");
    it.trackId = QStringLiteral("trk_v1");
    it.assetId = QStringLiteral("ast_interview");
    it.startFrame = 0;
    it.durationFrames = 120; // 4.0s
    it.sourceInFrame = 30;   // 1.0s
    it.props = VideoProps{};
    doc.items.push_back(it);

    return doc;
  }

private slots:
  void init() {
    project_.newProject(QStringLiteral("Suggest Test"), 1920, 1080, 30);
    project_.apply(Op(TrackAdd{.trackId = QStringLiteral("trk_v1"), .kind = TrackKind::Video, .name = QStringLiteral("V1")}));
    Item it;
    it.id = QStringLiteral("itm_1");
    it.trackId = QStringLiteral("trk_v1");
    it.assetId = QStringLiteral("ast_interview");
    it.startFrame = 0;
    it.durationFrames = 120;
    it.sourceInFrame = 30;
    it.props = VideoProps{};
    project_.apply(Op(ItemAdd{.item = it, .propsOmitted = false}));
  }

  void testDetectPauseAfterSplit() {
    SuggestionEngine engine;
    TimelineDoc doc = createDoc();

    // Mock speech span: silence from 1.0s to 1.5s (500ms pause)
    index::SpeechSpan pauseSpan;
    pauseSpan.assetId = QStringLiteral("ast_interview");
    pauseSpan.startSec = 1.0;
    pauseSpan.endSec = 1.5;
    pauseSpan.isSpeech = false; // silence

    const auto sug = engine.detectPauseAfterSplit(doc, QStringLiteral("itm_1"), {pauseSpan});
    QVERIFY(sug.has_value());
    QCOMPARE(sug->kind, SuggestionKind::GhostOp);
    QCOMPARE(sug->ghostKind, QStringLiteral("gap"));
    QVERIFY(sug->title.contains(QStringLiteral("Ripple delete pause")));
    QCOMPARE(sug->ops.size(), 1);
    QCOMPARE(sug->ops[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.trim"));
    QCOMPARE(sug->ops[0].value(QStringLiteral("ripple")).toBool(), true);
  }

  void testDetectLcutAudioExtension() {
    SuggestionEngine engine;
    TimelineDoc doc = createDoc();

    const auto sug = engine.detectLcutAudioExtension(doc, QStringLiteral("itm_1"), 250);
    QVERIFY(sug.has_value());
    QCOMPARE(sug->kind, SuggestionKind::GhostTrim);
    QCOMPARE(sug->ghostKind, QStringLiteral("trim"));
    QVERIFY(sug->title.contains(QStringLiteral("L-cut")));
    QCOMPARE(sug->durationMs, 250);
  }

  void testDetectBeatAlignment() {
    SuggestionEngine engine;
    TimelineDoc doc = createDoc();

    // Cut is at frame 120 = 4000ms. Beat is at 4050ms (diff = 50ms, within 80ms threshold)
    std::vector<double> beats{1000.0, 2000.0, 3000.0, 4050.0, 5000.0};

    const auto sug = engine.detectBeatAlignment(doc, QStringLiteral("itm_1"), false, beats, 80);
    QVERIFY(sug.has_value());
    QCOMPARE(sug->kind, SuggestionKind::GhostTrim);
    QVERIFY(sug->title.contains(QStringLiteral("Snap cut to beat")));
  }

  void testDetectBrollOverlay() {
    SuggestionEngine engine;
    TimelineDoc doc = createDoc();

    const auto sug = engine.detectBrollOverlay(doc, QStringLiteral("itm_1"), QStringLiteral("ast_broll"), QStringLiteral("trk_v2"), 60);
    QVERIFY(sug.has_value());
    QCOMPARE(sug->kind, SuggestionKind::GhostClip);
    QCOMPARE(sug->ghostKind, QStringLiteral("clip"));
    QCOMPARE(sug->targetTrackId, QStringLiteral("trk_v2"));
    QCOMPARE(sug->ops.size(), 1);
    QCOMPARE(sug->ops[0].value(QStringLiteral("type")).toString(), QStringLiteral("item.add"));
  }

  void testSuggestionLifecycle() {
    SuggestionEngine engine;
    QSignalSpy spyAvailable(&engine, &SuggestionEngine::suggestionAvailable);
    QSignalSpy spyDismissed(&engine, &SuggestionEngine::suggestionDismissed);
    QSignalSpy spyAccepted(&engine, &SuggestionEngine::suggestionAccepted);

    Suggestion s;
    s.id = QStringLiteral("sug_test");
    s.title = QStringLiteral("Test Suggestion");
    s.ops.append(QJsonObject{
      {QStringLiteral("type"), QStringLiteral("item.trim")},
      {QStringLiteral("itemId"), QStringLiteral("itm_1")},
      {QStringLiteral("edge"), QStringLiteral("out")},
      {QStringLiteral("frame"), 150},
      {QStringLiteral("ripple"), false}
    });

    engine.setSuggestion(s);
    QCOMPARE(spyAvailable.count(), 1);
    QVERIFY(engine.currentSuggestion().has_value());

    // Dismiss test
    engine.dismiss();
    QCOMPARE(spyDismissed.count(), 1);
    QVERIFY(!engine.currentSuggestion().has_value());

    // Accept test
    engine.setSuggestion(s);
    const bool ok = engine.accept(&project_);
    QVERIFY(ok);
    QCOMPARE(spyAccepted.count(), 1);
    QVERIFY(!engine.currentSuggestion().has_value());
    // Verify item was trimmed to out frame 150 (duration 150 frames with startFrame 0)
    QCOMPARE(project_.doc().items[0].durationFrames, 150);
  }
};

QTEST_GUILESS_MAIN(TstCopilotSuggest)
#include "tst_copilot_suggest.moc"
