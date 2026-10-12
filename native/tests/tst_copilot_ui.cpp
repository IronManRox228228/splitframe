#include "app/editor.h"
#include "app/player.h"
#include "app/timeline_view.h"
#include "core/timeline_doc.h"
#include "editor_testutil.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::app;
using namespace sf::editor;
using namespace sf::test;

class TestCopilotUi : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  std::unique_ptr<Player> player_;
  std::unique_ptr<Editor> editor_;

private slots:
  void init() {
    player_ = std::make_unique<Player>();
    player_->setSoftwareDecode(true);
    Editor::Options opts;
    opts.settingsFile = dir_.filePath(QStringLiteral("settings.ini"));
    opts.recoveryDir = dir_.filePath(QStringLiteral("recovery"));
    editor_ = std::make_unique<Editor>(*player_, opts);
    editor_->newProject(QStringLiteral("Copilot Test Project"), 1920, 1080, 30);
  }

  void cleanup() {
    editor_.reset();
    player_.reset();
  }

  void testInitialCopilotState() {
    QCOMPARE(editor_->lintIssueCount(), 0);
    QCOMPARE(editor_->suggestionCount(), 0);
    QCOMPARE(editor_->activeBranch(), QStringLiteral("main"));
    QVERIFY(editor_->branchList().contains(QStringLiteral("main")));
    QCOMPARE(editor_->vramStrained(), false);
  }

  void testLintWorkflowWithFlashFrame() {
    // Add track
    editor_->controller()->addTrack(QStringLiteral("video"));
    const auto& tracks = editor_->project()->doc().tracks;
    QVERIFY(!tracks.empty());
    const QString trackId = tracks.front().id;

    // Add normal clip
    Item normalItem;
    normalItem.id = QStringLiteral("item-normal");
    normalItem.trackId = trackId;
    normalItem.startFrame = 0;
    normalItem.durationFrames = 90;
    normalItem.props = VideoProps{};
    QVERIFY(editor_->project()->apply(ItemAdd{normalItem}));

    // Add flash frame: 1 frame duration (33ms at 30fps < 100ms threshold)
    Item flashItem;
    flashItem.id = QStringLiteral("item-flash");
    flashItem.trackId = trackId;
    flashItem.startFrame = 90;
    flashItem.durationFrames = 1;
    flashItem.props = VideoProps{};
    QVERIFY(editor_->project()->apply(ItemAdd{flashItem}));

    // Run linting
    editor_->runLint();
    QVERIFY(editor_->lintIssueCount() > 0);

    const QVariantList issues = editor_->lintIssues();
    bool foundFlash = false;
    QString flashIssueId;
    for (const auto& var : issues) {
      const QVariantMap iss = var.toMap();
      if (iss.value(QStringLiteral("ruleId")).toString() == QStringLiteral("flash_frame")) {
        foundFlash = true;
        flashIssueId = iss.value(QStringLiteral("id")).toString();
        break;
      }
    }
    QVERIFY(foundFlash);
    QVERIFY(!flashIssueId.isEmpty());

    // Apply auto-fix
    bool fixed = editor_->applyLintFix(flashIssueId);
    QVERIFY(fixed);

    // Verify issue resolved
    editor_->runLint();
    const auto updatedIssues = editor_->lintIssues();
    bool stillHasFlash = false;
    for (const auto& var : updatedIssues) {
      if (var.toMap().value(QStringLiteral("ruleId")).toString() == QStringLiteral("flash_frame")) {
        stillHasFlash = true;
      }
    }
    QVERIFY(!stillHasFlash);
  }

  void testBranchManagerLifecycle() {
    QVERIFY(editor_->createBranch(QStringLiteral("alt-cut-pacing")));
    QVERIFY(editor_->branchList().contains(QStringLiteral("alt-cut-pacing")));

    QVERIFY(editor_->switchBranch(QStringLiteral("alt-cut-pacing")));
    QCOMPARE(editor_->activeBranch(), QStringLiteral("alt-cut-pacing"));

    // Add track in branch
    editor_->controller()->addTrack(QStringLiteral("video"));
    QJsonObject diff = editor_->diffBranch(QStringLiteral("main"));
    QVERIFY(!diff.isEmpty());

    // Switch back to main
    QVERIFY(editor_->switchBranch(QStringLiteral("main")));
    QCOMPARE(editor_->activeBranch(), QStringLiteral("main"));
  }

  void testSuggestionLifecycle() {
    copilot::Suggestion s;
    s.id = QStringLiteral("sugg-test-01");
    s.title = QStringLiteral("Ripple delete gap");
    s.targetTrackId = QStringLiteral("trk-1");
    s.startMs = 500;
    s.durationMs = 1500;
    s.ghostKind = QStringLiteral("trim");
    s.confidence = 0.95f;

    editor_->suggestionEngine()->setSuggestion(s);
    QCOMPARE(editor_->suggestionCount(), 1);

    const auto suggs = editor_->suggestions();
    QCOMPARE(suggs.size(), 1);
    QCOMPARE(suggs[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("sugg-test-01"));

    editor_->rejectSuggestion(QStringLiteral("sugg-test-01"));
    QCOMPARE(editor_->suggestionCount(), 0);
  }

  void testZetarisMediaIntegration() {
    index::FederatedAsset asset;
    asset.id = QStringLiteral("asset-zetaris-tokyo");
    asset.uri = QStringLiteral("s3://broll/tokyo_sunset.mp4");
    asset.name = QStringLiteral("Tokyo Sunset");
    asset.storageType = index::FederatedStorageType::CloudS3;
    asset.tags = {QStringLiteral("japan"), QStringLiteral("sunset")};
    asset.width = 3840;
    asset.height = 2160;

    editor_->zetarisProvider()->setMockAssets({asset});

    QVariantList res = editor_->searchFederatedMedia(QStringLiteral("sunset"), {QStringLiteral("japan")});
    QCOMPARE(res.size(), 1);
    QCOMPARE(res[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("asset-zetaris-tokyo"));

    QString importedId = editor_->importFederatedAsset(QStringLiteral("asset-zetaris-tokyo"));
    QCOMPARE(importedId, QStringLiteral("asset-zetaris-tokyo"));
    QVERIFY(editor_->project()->asset(importedId) != nullptr);
  }

  void testVramStrainedToggle() {
    QCOMPARE(editor_->vramStrained(), false);
    editor_->setVramStrained(true);
    QCOMPARE(editor_->vramStrained(), true);
    editor_->setVramStrained(false);
    QCOMPARE(editor_->vramStrained(), false);
  }

  void testTimelineViewPaintRobustness() {
    TimelineView view;
    view.setEditor(editor_.get());
    view.setProject(editor_->project());
    view.setModel(editor_->timeline());
    view.setController(editor_->controller());
    view.setSize(QSizeF(1200, 600));

    // Paint to QPixmap
    QPixmap pixmap(1200, 600);
    pixmap.fill(Qt::black);
    {
      QPainter p(&pixmap);
      view.paint(&p);
    }

    // Stress extreme zooms and offsets
    view.setPxPerFrame(0.005);
    view.setScrollX(-500);
    {
      QPainter p(&pixmap);
      view.paint(&p);
    }

    view.setPxPerFrame(50.0);
    view.setScrollX(500000);
    {
      QPainter p(&pixmap);
      view.paint(&p);
    }

    QVERIFY(!pixmap.isNull());
  }
};

QTEST_MAIN(TestCopilotUi)
#include "tst_copilot_ui.moc"
