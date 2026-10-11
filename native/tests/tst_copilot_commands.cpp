#include "copilot/copilot_commands.h"
#include "editor/project.h"
#include "engine/engine.h"

#include <QTest>

using namespace sf;
using namespace sf::engine;
using namespace sf::copilot;

class TstCopilotCommands : public QObject {
  Q_OBJECT

private:
  Engine engine_;
  LintEngine lintEngine_;
  SuggestionEngine suggestionEngine_;
  BranchManager branchManager_;
  ReviewEngine reviewEngine_;

private slots:
  void init() {
    engine_.createSession();
    auto* sess = engine_.session();
    QVERIFY(sess && sess->project);

    sess->project->newProject(QStringLiteral("Cmd Test"), 1920, 1080, 30);
    // Add V1 track
    sess->project->apply(Op(TrackAdd{
      .trackId = QStringLiteral("trk_v1"),
      .kind = TrackKind::Video,
      .name = QStringLiteral("V1")
    }));
    // Add item 1
    Item it;
    it.id = QStringLiteral("itm_1");
    it.trackId = QStringLiteral("trk_v1");
    it.startFrame = 0;
    it.durationFrames = 60;
    it.props = VideoProps{};
    sess->project->apply(Op(ItemAdd{.item = it, .propsOmitted = false}));

    branchManager_.attachProject(sess->project);

    registerCopilotCommands(&engine_, &lintEngine_, &suggestionEngine_, &branchManager_, &reviewEngine_, nullptr);
  }

  void testLintCommands() {
    auto* sess = engine_.session();
    // Add flash frame item to trigger lint issue
    Item flash;
    flash.id = QStringLiteral("itm_flash");
    flash.trackId = QStringLiteral("trk_v1");
    flash.startFrame = 60;
    flash.durationFrames = 2; // flash frame
    flash.props = VideoProps{};
    sess->project->apply(Op(ItemAdd{.item = flash, .propsOmitted = false}));

    // copilot.lint.run
    const auto res = engine_.call(QStringLiteral("copilot.lint.run"));
    QVERIFY(res.ok);
    QVERIFY(res.value.toObject().value(QStringLiteral("count")).toInteger() >= 1);

    // copilot.lint.fix
    const auto fixRes = engine_.call(QStringLiteral("copilot.lint.fix"), QJsonObject{
      {QStringLiteral("issueId"), QStringLiteral("lint-flash-itm_flash")}
    });
    QVERIFY(fixRes.ok);
    QVERIFY(fixRes.value.toObject().value(QStringLiteral("fixed")).toBool());

    // Verify flash frame item was removed
    QCOMPARE(sess->project->doc().items.size(), 1);
  }

  void testSuggestCommands() {
    // Initial get: no suggestion
    auto getRes = engine_.call(QStringLiteral("copilot.suggest.get"));
    QVERIFY(getRes.ok);
    QCOMPARE(getRes.value.toObject().value(QStringLiteral("hasSuggestion")).toBool(), false);

    // Set suggestion
    Suggestion s;
    s.id = QStringLiteral("sug_cmd");
    s.title = QStringLiteral("Trim Item");
    s.ops.append(QJsonObject{
      {QStringLiteral("type"), QStringLiteral("item.trim")},
      {QStringLiteral("itemId"), QStringLiteral("itm_1")},
      {QStringLiteral("edge"), QStringLiteral("out")},
      {QStringLiteral("frame"), 90},
      {QStringLiteral("ripple"), false}
    });
    suggestionEngine_.setSuggestion(s);

    // Get suggestion
    getRes = engine_.call(QStringLiteral("copilot.suggest.get"));
    QVERIFY(getRes.ok);
    QCOMPARE(getRes.value.toObject().value(QStringLiteral("hasSuggestion")).toBool(), true);
    QCOMPARE(getRes.value.toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Trim Item"));

    // Accept suggestion
    const auto acceptRes = engine_.call(QStringLiteral("copilot.suggest.accept"));
    QVERIFY(acceptRes.ok);
    QVERIFY(acceptRes.value.toObject().value(QStringLiteral("accepted")).toBool());

    // Dismiss command on idle slot
    const auto dismissRes = engine_.call(QStringLiteral("copilot.suggest.dismiss"));
    QVERIFY(dismissRes.ok);
    QVERIFY(dismissRes.value.toObject().value(QStringLiteral("dismissed")).toBool());
  }

  void testReviewCommand() {
    const auto res = engine_.call(QStringLiteral("copilot.review.run"));
    QVERIFY(res.ok);
    QVERIFY(res.value.toObject().value(QStringLiteral("passed")).toBool());
    QVERIFY(res.value.toObject().value(QStringLiteral("score")).toDouble() >= 0.70);
  }

  void testBranchCommands() {
    // branch.list (initially main)
    auto listRes = engine_.call(QStringLiteral("branch.list"));
    QVERIFY(listRes.ok);
    QCOMPARE(listRes.value.toObject().value(QStringLiteral("active")).toString(), QStringLiteral("main"));

    // branch.create
    const auto createRes = engine_.call(QStringLiteral("branch.create"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("agent/cut")},
      {QStringLiteral("author"), QStringLiteral("agent:editor")}
    });
    QVERIFY(createRes.ok);
    QVERIFY(createRes.value.toObject().value(QStringLiteral("created")).toBool());

    // branch.switch
    const auto switchRes = engine_.call(QStringLiteral("branch.switch"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("agent/cut")}
    });
    QVERIFY(switchRes.ok);
    QVERIFY(switchRes.value.toObject().value(QStringLiteral("switched")).toBool());

    // branch.diff
    const auto diffRes = engine_.call(QStringLiteral("branch.diff"), QJsonObject{
      {QStringLiteral("nameA"), QStringLiteral("main")},
      {QStringLiteral("nameB"), QStringLiteral("agent/cut")}
    });
    QVERIFY(diffRes.ok);

    // Switch back to main
    engine_.call(QStringLiteral("branch.switch"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("main")}
    });

    // branch.delete
    const auto delRes = engine_.call(QStringLiteral("branch.delete"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("agent/cut")}
    });
    QVERIFY(delRes.ok);
    QVERIFY(delRes.value.toObject().value(QStringLiteral("deleted")).toBool());
  }
};

QTEST_GUILESS_MAIN(TstCopilotCommands)
#include "tst_copilot_commands.moc"
