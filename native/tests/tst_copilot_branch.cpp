#include "copilot/branch_manager.h"
#include "editor/project.h"

#include <QTest>

using namespace sf;
using namespace sf::editor;
using namespace sf::copilot;

class TstCopilotBranch : public QObject {
  Q_OBJECT

private:
  sf::editor::Project project_;

private slots:
  void init() {
    project_.newProject(QStringLiteral("Branch Test"), 1920, 1080, 30);
    // Add V1 track
    project_.apply(Op(TrackAdd{
      .trackId = QStringLiteral("trk_v1"),
      .kind = TrackKind::Video,
      .name = QStringLiteral("V1")
    }));
    // Add item 1
    Item it1;
    it1.id = QStringLiteral("itm_1");
    it1.trackId = QStringLiteral("trk_v1");
    it1.startFrame = 0;
    it1.durationFrames = 60;
    it1.props = VideoProps{};
    project_.apply(Op(ItemAdd{.item = it1, .propsOmitted = false}));
  }

  void testInitialState() {
    BranchManager bm(&project_);
    QCOMPARE(bm.activeBranch(), QStringLiteral("main"));
    const auto branches = bm.listBranches();
    QCOMPARE(branches.size(), 1);
    QCOMPARE(branches[0].name, QStringLiteral("main"));
  }

  void testCreateBranch() {
    BranchManager bm(&project_);
    const bool ok = bm.createBranch(QStringLiteral("agent/tight"), QStringLiteral("agent:editor"));
    QVERIFY(ok);

    const auto branches = bm.listBranches();
    QCOMPARE(branches.size(), 2);

    const auto b = bm.getBranch(QStringLiteral("agent/tight"));
    QVERIFY(b.has_value());
    QCOMPARE(b->author, QStringLiteral("agent:editor"));
    QCOMPARE(b->baseBranch, QStringLiteral("main"));

    // Reject duplicate name
    QVERIFY(!bm.createBranch(QStringLiteral("agent/tight")));
    // Reject empty name
    QVERIFY(!bm.createBranch(QStringLiteral("")));
  }

  void testSwitchAndModify() {
    BranchManager bm(&project_);
    QVERIFY(bm.createBranch(QStringLiteral("agent/pacing")));
    QVERIFY(bm.switchBranch(QStringLiteral("agent/pacing")));
    QCOMPARE(bm.activeBranch(), QStringLiteral("agent/pacing"));

    // Modify timeline on agent/pacing branch by adding item 2
    Item it2;
    it2.id = QStringLiteral("itm_2");
    it2.trackId = QStringLiteral("trk_v1");
    it2.startFrame = 60;
    it2.durationFrames = 30;
    it2.props = VideoProps{};
    project_.apply(Op(ItemAdd{.item = it2, .propsOmitted = false}));
    QCOMPARE(project_.doc().items.size(), 2);

    // Switch back to main branch
    QVERIFY(bm.switchBranch(QStringLiteral("main")));
    QCOMPARE(bm.activeBranch(), QStringLiteral("main"));
    // On main, there should still only be 1 item!
    QCOMPARE(project_.doc().items.size(), 1);

    // Switch back to agent/pacing
    QVERIFY(bm.switchBranch(QStringLiteral("agent/pacing")));
    QCOMPARE(project_.doc().items.size(), 2);
  }

  void testDiffBranches() {
    BranchManager bm(&project_);
    QVERIFY(bm.createBranch(QStringLiteral("alt/cut")));
    QVERIFY(bm.switchBranch(QStringLiteral("alt/cut")));

    // Add item 2 on alt/cut
    Item it2;
    it2.id = QStringLiteral("itm_2");
    it2.trackId = QStringLiteral("trk_v1");
    it2.startFrame = 60;
    it2.durationFrames = 30; // 30 frames at 30fps = 1000ms duration addition
    it2.props = VideoProps{};
    project_.apply(Op(ItemAdd{.item = it2, .propsOmitted = false}));

    // Diff main vs alt/cut
    const auto diff = bm.diffBranches(QStringLiteral("main"), QStringLiteral("alt/cut"));
    QCOMPARE(diff.durationDeltaMs, 1000); // 90 frames (3000ms) - 60 frames (2000ms) = 1000ms
    QCOMPARE(diff.addedItemIds.size(), 1);
    QCOMPARE(diff.addedItemIds[0], QStringLiteral("itm_2"));
    QCOMPARE(diff.removedItemIds.size(), 0);
  }

  void testMergeBranch() {
    BranchManager bm(&project_);
    QVERIFY(bm.createBranch(QStringLiteral("agent/feature")));
    QVERIFY(bm.switchBranch(QStringLiteral("agent/feature")));

    // Add item 2
    Item it2;
    it2.id = QStringLiteral("itm_2");
    it2.trackId = QStringLiteral("trk_v1");
    it2.startFrame = 60;
    it2.durationFrames = 30;
    it2.props = VideoProps{};
    project_.apply(Op(ItemAdd{.item = it2, .propsOmitted = false}));

    // Switch back to main
    QVERIFY(bm.switchBranch(QStringLiteral("main")));
    QCOMPARE(project_.doc().items.size(), 1);

    // Merge agent/feature into main
    const bool merged = bm.mergeBranch(QStringLiteral("agent/feature"));
    QVERIFY(merged);
    // Main now has item 2 merged!
    QCOMPARE(project_.doc().items.size(), 2);
  }

  void testDeleteBranch() {
    BranchManager bm(&project_);
    QVERIFY(bm.createBranch(QStringLiteral("temp")));
    // Cannot delete active branch
    QVERIFY(!bm.deleteBranch(QStringLiteral("main")));
    // Cannot delete nonexistent branch
    QVERIFY(!bm.deleteBranch(QStringLiteral("nonexistent")));

    // Delete temp
    QVERIFY(bm.deleteBranch(QStringLiteral("temp")));
    QCOMPARE(bm.listBranches().size(), 1);
  }
};

QTEST_GUILESS_MAIN(TstCopilotBranch)
#include "tst_copilot_branch.moc"
