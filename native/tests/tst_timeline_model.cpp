#include "editor/timeline_model.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

class TstTimelineModel : public QObject {
  Q_OBJECT
private slots:
  void exposesItemsOrderedByTrackThenStart() {
    Project p;
    fillProject(p);
    TimelineModel m(p);
    QCOMPARE(m.rowCount(), 4);
    // tracks are text, video, audio: t1, v1, v2, a1
    QCOMPARE(m.data(m.index(0), TimelineModel::ItemIdRole).toString(), QStringLiteral("itm_t1"));
    QCOMPARE(m.data(m.index(1), TimelineModel::ItemIdRole).toString(), QStringLiteral("itm_v1"));
    QCOMPARE(m.data(m.index(2), TimelineModel::ItemIdRole).toString(), QStringLiteral("itm_v2"));
    QCOMPARE(m.data(m.index(3), TimelineModel::ItemIdRole).toString(), QStringLiteral("itm_a1"));
    QCOMPARE(m.data(m.index(2), TimelineModel::StartRole).toLongLong(), 120);
    QCOMPARE(m.data(m.index(2), TimelineModel::EndRole).toLongLong(), 210);
    QCOMPARE(m.data(m.index(2), TimelineModel::TypeRole).toString(), QStringLiteral("video"));
    QCOMPARE(m.data(m.index(2), TimelineModel::NameRole).toString(), QStringLiteral("clip.mp4"));
    QCOMPARE(m.data(m.index(0), TimelineModel::NameRole).toString(), QStringLiteral("Hello")); // text shows its text
    QCOMPARE(m.data(m.index(2), TimelineModel::TrackIndexRole).toInt(), 1);
    QVERIFY(m.data(m.index(2), TimelineModel::ColorRole).value<QColor>().isValid());
  }
  void roleNamesAreUsableFromQml() {
    Project p;
    TimelineModel m(p);
    const auto names = m.roleNames().values();
    for (const char* n : {"itemId", "trackIndex", "start", "duration", "type", "name", "selected", "locked", "color"}) QVERIFY(names.contains(n));
  }
  void valueChangesUpdateRowsInPlace() {
    Project p;
    fillProject(p);
    TimelineModel m(p);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
    QVERIFY(p.apply(ItemMove{QStringLiteral("itm_v2"), std::nullopt, 150}));
    QCOMPARE(reset.count(), 0);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(m.data(m.index(m.rowOf(QStringLiteral("itm_v2"))), TimelineModel::StartRole).toLongLong(), 150);
  }
  void addRemoveAndReorderReset() {
    Project p;
    fillProject(p);
    TimelineModel m(p);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    QVERIFY(p.apply(ItemRemove{{QStringLiteral("itm_v1")}, false}));
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(reset.count(), 1);
    QVERIFY(p.undo());
    QCOMPARE(m.rowCount(), 4);
    QCOMPARE(reset.count(), 2);
    // moving v2 before v1 changes the order: reset too
    QVERIFY(p.apply(ItemMove{QStringLiteral("itm_v2"), std::nullopt, 0}));
    QCOMPARE(m.data(m.index(1), TimelineModel::ItemIdRole).toString(), QStringLiteral("itm_v1"));
  }
  void selectionAndLockShowUp() {
    Project p;
    fillProject(p);
    TimelineModel m(p);
    p.setSelection({QStringLiteral("itm_a1")});
    QVERIFY(m.data(m.index(m.rowOf(QStringLiteral("itm_a1"))), TimelineModel::SelectedRole).toBool());
    TrackUpdate lock;
    lock.trackId = trackNamed(p.doc(), TrackKind::Audio).id;
    lock.patch.locked = true;
    QVERIFY(p.apply(lock));
    QVERIFY(m.data(m.index(m.rowOf(QStringLiteral("itm_a1"))), TimelineModel::LockedRole).toBool());
    QVERIFY(!m.data(m.index(m.rowOf(QStringLiteral("itm_v1"))), TimelineModel::LockedRole).toBool());
  }
  void missingMediaIsFlagged() {
    Project p;
    fillProject(p);
    TimelineModel m(p);
    Asset a = *p.asset(QStringLiteral("ast_video"));
    a.status = AssetStatus::Missing;
    p.updateAsset(a);
    QVERIFY(m.data(m.index(m.rowOf(QStringLiteral("itm_v1"))), TimelineModel::MissingRole).toBool());
  }
  void handlesHundredsOfItems() {
    Project p;
    fillProject(p);
    std::vector<Op> ops;
    for (int i = 0; i < 600; ++i) {
      ItemInit init;
      init.id = QStringLiteral("itm_bulk%1").arg(i, 4, 10, QLatin1Char('0'));
      init.trackId = trackNamed(p.doc(), TrackKind::Video).id;
      init.startFrame = 300 + i * 10;
      init.durationFrames = 8;
      init.assetId = QStringLiteral("ast_video");
      init.sourceInFrame = 0;
      ops.emplace_back(ItemAdd{createItem(ItemType::Video, init), false});
    }
    TimelineModel m(p);
    QVERIFY(p.apply(ops));
    QCOMPARE(m.rowCount(), 604);
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 20; ++i) QVERIFY(p.apply(ItemMove{QStringLiteral("itm_bulk0005"), std::nullopt, 1000 + i}));
    QVERIFY2(t.elapsed() < 3000, "20 edits on 600 clips took too long");
  }
  void trackModelFollowsTracks() {
    Project p;
    fillProject(p);
    TrackModel t(p);
    QCOMPARE(t.rowCount(), 3);
    QCOMPARE(t.data(t.index(1), TrackModel::KindRole).toString(), QStringLiteral("video"));
    QCOMPARE(t.data(t.index(1), TrackModel::HeightRole).toInt(), 64);
    QCOMPARE(t.data(t.index(1), TrackModel::ItemCountRole).toInt(), 2);
    QSignalSpy changed(&t, &QAbstractItemModel::dataChanged);
    TrackUpdate u;
    u.trackId = t.data(t.index(1), TrackModel::TrackIdRole).toString();
    u.patch.muted = true;
    QVERIFY(p.apply(u));
    QCOMPARE(changed.count(), 1);
    QVERIFY(t.data(t.index(1), TrackModel::MutedRole).toBool());
    TrackAdd add;
    add.trackId = QStringLiteral("trk_new");
    add.kind = TrackKind::Audio;
    add.name = QStringLiteral("More audio");
    QVERIFY(p.apply(add));
    QCOMPARE(t.rowCount(), 4);
    QCOMPARE(t.data(t.index(3), TrackModel::NameRole).toString(), QStringLiteral("More audio"));
  }
};

} // namespace tst
using tst::TstTimelineModel;
QTEST_GUILESS_MAIN(TstTimelineModel)
#include "tst_timeline_model.moc"
