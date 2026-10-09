#include "editor/project.h"
#include "editor/recent_projects.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

class TstProject : public QObject {
  Q_OBJECT
private slots:
  void newProjectIsCleanAndUntitled() {
    Project p;
    QVERIFY(!p.isDirty());
    QVERIFY(p.path().isEmpty());
    QCOMPARE(p.displayName(), QStringLiteral("Untitled video"));
    QCOMPARE(p.doc().tracks.size(), size_t(3));
  }
  void editMakesDirtyAndTitleShowsIt() {
    Project p;
    QSignalSpy spy(&p, &Project::dirtyChanged);
    QVERIFY(p.apply(ProjectRename{QStringLiteral("Renamed")}, QStringLiteral("Rename")));
    QVERIFY(p.isDirty());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(p.displayName(), QStringLiteral("Renamed *"));
  }
  void undoBackToSavedStateIsClean() {
    QTemporaryDir dir;
    Project p;
    QString err;
    QVERIFY(p.saveAs(dir.filePath(QStringLiteral("a.json")), &err));
    QVERIFY(!p.isDirty());
    QVERIFY(p.apply(ProjectRename{QStringLiteral("X")}));
    QVERIFY(p.isDirty());
    QVERIFY(p.undo());
    QVERIFY(!p.isDirty());
    QVERIFY(p.redo());
    QVERIFY(p.isDirty());
  }
  void saveOpenRoundTrip() {
    QTemporaryDir dir;
    Project p;
    fillProject(p);
    QVERIFY(p.apply(ProjectRename{QStringLiteral("Round trip")}));
    const QString file = dir.filePath(QStringLiteral("rt.json"));
    QString err;
    QVERIFY2(p.saveAs(file, &err), qPrintable(err));
    QCOMPARE(p.path(), QFileInfo(file).absoluteFilePath());
    QVERIFY(!p.isDirty());

    Project q;
    QVERIFY2(q.open(file, &err), qPrintable(err));
    QVERIFY(q.doc().items == p.doc().items);
    QVERIFY(q.doc().tracks == p.doc().tracks);
    QCOMPARE(q.assets().size(), size_t(3));
    QVERIFY(!q.isDirty());
    QVERIFY(!q.canUndo()); // history starts fresh
  }
  void fileIsTheBundleFormatCoreReads() {
    QTemporaryDir dir;
    Project p;
    fillProject(p);
    QVERIFY(p.saveAs(dir.filePath(QStringLiteral("f.json"))));
    const ProjectBundle b = loadProjectBundle(dir.filePath(QStringLiteral("f.json")));
    QCOMPARE(b.doc.items.size(), size_t(4));
    QCOMPARE(b.assets.size(), size_t(3));
  }
  void opensBareTimelineDoc() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("bare.json"));
    saveTimelineDoc(file, createEmptyDoc({.id = QStringLiteral("prj_bare"), .name = QStringLiteral("Bare")}));
    Project p;
    QString err;
    QVERIFY2(p.open(file, &err), qPrintable(err));
    QCOMPARE(p.name(), QStringLiteral("Bare"));
    QVERIFY(p.assets().empty());
  }
  void openFailsCleanly() {
    QTemporaryDir dir;
    QFile f(dir.filePath(QStringLiteral("bad.json")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"nope\":1}");
    f.close();
    Project p;
    p.apply(ProjectRename{QStringLiteral("keep")});
    QString err;
    QVERIFY(!p.open(f.fileName(), &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(p.name(), QStringLiteral("keep")); // untouched
    QVERIFY(!p.open(dir.filePath(QStringLiteral("missing.json")), &err));
  }
  void relativeAssetPathsResolveAgainstTheFile() {
    QTemporaryDir dir;
    Project p;
    p.addAsset(makeAsset(QStringLiteral("ast_rel"), AssetKind::Video, QStringLiteral("v.mp4"), 1000, QStringLiteral("media/v.mp4")));
    const QString file = dir.filePath(QStringLiteral("rel.json"));
    QVERIFY(p.saveAs(file));
    Project q;
    QVERIFY(q.open(file));
    QCOMPARE(QFileInfo(q.assets()[0].path).absoluteFilePath(), QFileInfo(dir.filePath(QStringLiteral("media/v.mp4"))).absoluteFilePath());
  }
  void groupedEditsAreOneUndoStep() {
    Project p;
    fillProject(p);
    p.beginGroup(QStringLiteral("drag"));
    for (double o : {0.9, 0.7, 0.4}) {
      ItemUpdate u;
      u.itemId = QStringLiteral("itm_v1");
      TransformPatch t;
      t.opacity = o;
      u.patch.transform = t;
      QVERIFY(p.apply(u));
    }
    p.endGroup();
    QCOMPARE(itemOf(p, QStringLiteral("itm_v1")).transform.opacity, 0.4);
    QVERIFY(p.undo());
    QCOMPARE(itemOf(p, QStringLiteral("itm_v1")).transform.opacity, 1.0);
    QVERIFY(!p.canUndo());
    QVERIFY(p.redo());
    QCOMPARE(itemOf(p, QStringLiteral("itm_v1")).transform.opacity, 0.4);
  }
  void lockedTrackRejectsEditsAndKeepsDoc() {
    Project p;
    fillProject(p);
    TrackUpdate lock;
    lock.trackId = trackNamed(p.doc(), TrackKind::Video).id;
    lock.patch.locked = true;
    QVERIFY(p.apply(lock));
    QSignalSpy rejected(&p, &Project::editRejected);
    const TimelineDoc before = p.doc();
    QVERIFY(!p.apply(ItemMove{QStringLiteral("itm_v1"), std::nullopt, 5}));
    QCOMPARE(rejected.count(), 1);
    QVERIFY(p.doc() == before);
  }
  void undoStillWorksAfterTheTrackWasLocked() {
    Project p;
    fillProject(p);
    QVERIFY(p.apply(ItemMove{QStringLiteral("itm_v1"), std::nullopt, 5}));
    TrackUpdate lock;
    lock.trackId = trackNamed(p.doc(), TrackKind::Video).id;
    lock.patch.locked = true;
    QVERIFY(p.apply(lock));
    QVERIFY(p.undo()); // the lock
    QVERIFY(p.undo()); // the move
    QCOMPARE(itemOf(p, QStringLiteral("itm_v1")).startFrame, Frame(0));
  }
  void selectionFollowsTheDocument() {
    Project p;
    fillProject(p);
    p.setSelection({QStringLiteral("itm_v1"), QStringLiteral("itm_v2")});
    QVERIFY(p.apply(ItemRemove{{QStringLiteral("itm_v1")}, false}));
    QCOMPARE(p.selection(), QStringList{QStringLiteral("itm_v2")});
    p.select(QStringLiteral("itm_a1"), true);
    QCOMPARE(p.selection().size(), 2);
    p.select(QStringLiteral("itm_a1"), true);
    QCOMPARE(p.selection().size(), 1);
  }
  void removeAssetDropsHistoryAndRefusesWhileUsed() {
    Project p;
    fillProject(p);
    QVERIFY(!p.removeAsset(QStringLiteral("ast_video"))); // still on the timeline
    QVERIFY(p.apply(ItemRemove{{QStringLiteral("itm_v1"), QStringLiteral("itm_v2")}, false}));
    QVERIFY(p.removeAsset(QStringLiteral("ast_video")));
    QVERIFY(!p.asset(QStringLiteral("ast_video")));
    QVERIFY(!p.canUndo()); // the only group referenced the asset
  }
  void autosaveWritesRecoveryAndSaveClearsIt() {
    QTemporaryDir dir;
    Project p;
    p.setRecoveryDir(dir.filePath(QStringLiteral("rec")));
    p.setAutosaveInterval(0);
    QVERIFY(!p.autosaveNow()); // clean: nothing to write
    p.apply(ProjectRename{QStringLiteral("Unsaved work")});
    QVERIFY(p.autosaveNow());
    const auto list = p.recoveries();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list[0].name, QStringLiteral("Unsaved work"));
    QVERIFY(list[0].originalPath.isEmpty());
    QVERIFY(p.saveAs(dir.filePath(QStringLiteral("final.json"))));
    QCOMPARE(p.recoveries().size(), 0);
  }
  void autosaveTimerFiresAfterQuietPeriod() {
    QTemporaryDir dir;
    Project p;
    p.setRecoveryDir(dir.filePath(QStringLiteral("rec")));
    p.setAutosaveInterval(30);
    p.apply(ProjectRename{QStringLiteral("T")});
    QTRY_COMPARE_WITH_TIMEOUT(p.recoveries().size(), 1, 3000);
  }
  void recoveryRestoresDirtyProjectWithItsPath() {
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("proj.json"));
    {
      Project p;
      p.setRecoveryDir(dir.filePath(QStringLiteral("rec")));
      QVERIFY(p.saveAs(file));
      p.apply(ProjectRename{QStringLiteral("After crash")});
      QVERIFY(p.autosaveNow());
      // simulated crash: the object goes away without saving
    }
    // make the autosave strictly newer than the saved file
    QFile saved(file);
    QVERIFY(saved.open(QIODevice::ReadWrite));
    QVERIFY(saved.setFileTime(QDateTime::currentDateTime().addSecs(-60), QFileDevice::FileModificationTime));
    saved.close();
    Project q;
    q.setRecoveryDir(dir.filePath(QStringLiteral("rec")));
    const auto rec = q.recoveryFor(file);
    QVERIFY(rec.has_value());
    QString err;
    QVERIFY2(q.openRecovery(*rec, &err), qPrintable(err));
    QCOMPARE(q.name(), QStringLiteral("After crash"));
    QVERIFY(q.isDirty());
    QCOMPARE(q.path(), QFileInfo(file).absoluteFilePath());
    QVERIFY(q.save());
    QVERIFY(!q.isDirty());
    QVERIFY(!q.recoveryFor(file).has_value());
  }
  void recentProjectsKeepNewestFirstAndCap() {
    QTemporaryDir dir;
    RecentProjects r(dir.filePath(QStringLiteral("s.ini")));
    for (int i = 0; i < 13; ++i) r.add(dir.filePath(QStringLiteral("p%1.json").arg(i)));
    QCOMPARE(r.paths().size(), RecentProjects::kMax);
    QVERIFY(r.paths().first().endsWith(QStringLiteral("p12.json")));
    r.add(dir.filePath(QStringLiteral("p5.json")));
    QVERIFY(r.paths().first().endsWith(QStringLiteral("p5.json")));
    QCOMPARE(r.paths().size(), RecentProjects::kMax);
    r.remove(dir.filePath(QStringLiteral("p5.json")));
    QCOMPARE(r.paths().size(), RecentProjects::kMax - 1);
    RecentProjects again(dir.filePath(QStringLiteral("s.ini")));
    QCOMPARE(again.paths(), r.paths()); // persisted
    QCOMPARE(again.entries().first().toMap().value(QStringLiteral("exists")).toBool(), false);
  }
};

} // namespace tst
using tst::TstProject;
QTEST_GUILESS_MAIN(TstProject)
#include "tst_project.moc"
