#include "editor/media_pool.h"
#include "editor_testutil.h"

#include <QSignalSpy>

namespace tst {
using namespace sf;
using namespace sf::editor;
using namespace sf::test;
using sf::editor::Project; // not sf::Project

class TstMediaPool : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString video_, audio_, image_;

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    video_ = dir_.filePath(QStringLiteral("clip.mp4"));
    audio_ = dir_.filePath(QStringLiteral("tone.wav"));
    image_ = dir_.filePath(QStringLiteral("still.png"));
    QString log;
    QVERIFY2(makeIndexClip(video_, {QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-q:v"), QStringLiteral("4"), QStringLiteral("-g"), QStringLiteral("10")},
                           320, 240, 25, 4, {}, &log), qPrintable(log));
    QVERIFY2(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=2"), QStringLiteral("-c:a"),
                        QStringLiteral("pcm_s16le"), audio_}, &log), qPrintable(log));
    QImage img(200, 100, QImage::Format_RGB32);
    img.fill(Qt::darkCyan);
    QVERIFY(img.save(image_));
  }

  void importProbesAndBuildsPreviews() {
    Project p;
    MediaPool pool(p);
    QSignalSpy previews(&pool, &MediaPool::previewReady);
    const ImportSummary s = pool.import({video_, audio_, image_});
    QCOMPARE(s.added.size(), 3);
    QCOMPARE(pool.rowCount(), 3);
    // right away they exist as "importing"
    QCOMPARE(p.asset(s.added[0])->status, AssetStatus::Importing);
    QVERIFY(pool.waitForIdle());
    QCOMPARE(pool.pending(), 0);

    const Asset* v = nullptr;
    const Asset* a = nullptr;
    const Asset* im = nullptr;
    for (const Asset& x : p.assets()) {
      if (x.originalName == QStringLiteral("clip.mp4")) v = &x;
      if (x.originalName == QStringLiteral("tone.wav")) a = &x;
      if (x.originalName == QStringLiteral("still.png")) im = &x;
    }
    QVERIFY(v && a && im);
    QCOMPARE(v->status, AssetStatus::Analyzed);
    QCOMPARE(v->kind, AssetKind::Video);
    QCOMPARE(v->width, std::int64_t(320));
    QCOMPARE(v->height, std::int64_t(240));
    QVERIFY(std::abs(v->durationMs - 4000) < 200);
    QVERIFY(v->fps && std::abs(*v->fps - 25) < 0.5);
    QVERIFY(v->sizeBytes > 0);
    QCOMPARE(a->kind, AssetKind::Audio);
    QVERIFY(std::abs(a->durationMs - 2000) < 100);
    QVERIFY(a->hasAudio);
    QCOMPARE(im->kind, AssetKind::Image);
    QCOMPARE(im->width, std::int64_t(200));

    const auto vp = pool.preview(v->id);
    QVERIFY(vp);
    QVERIFY(vp->strip.size() >= 4);
    QVERIFY(!vp->poster.isNull());
    QVERIFY(vp->poster.width() <= 160 && vp->poster.height() <= 90);
    const auto ap = pool.preview(a->id);
    QVERIFY(ap && ap->peaks.bucketCount() > 100);
    QVERIFY(ap->peaks.maxAt(10) > 0.1f); // a real sine, not silence
    QVERIFY(pool.preview(im->id) && !pool.preview(im->id)->poster.isNull());
    QCOMPARE(previews.count(), 3);

    // list model data
    const QModelIndex first = pool.index(0);
    QVERIFY(!pool.data(first, MediaPool::NameRole).toString().isEmpty());
    QVERIFY(pool.data(first, MediaPool::ThumbRole).toString().startsWith(QStringLiteral("image://pool/")));
    QCOMPARE(pool.data(first, MediaPool::StatusRole).toString(), QStringLiteral("analyzed"));
  }

  void duplicatesAndUnsupportedFilesAreReported() {
    Project p;
    MediaPool pool(p);
    QFile txt(dir_.filePath(QStringLiteral("notes.txt")));
    QVERIFY(txt.open(QIODevice::WriteOnly));
    txt.write("hi");
    txt.close();
    ImportSummary s = pool.import({video_, txt.fileName(), dir_.filePath(QStringLiteral("nope.mp4"))});
    QCOMPARE(s.added.size(), 1);
    QCOMPARE(s.skipped.size(), 2);
    s = pool.import({video_});
    QCOMPARE(s.added.size(), 0);
    QCOMPARE(s.duplicates.size(), 1);
    QCOMPARE(p.assets().size(), size_t(1));
    QVERIFY(pool.waitForIdle());
  }

  void corruptMediaFailsWithoutBlockingTheRest() {
    Project p;
    MediaPool pool(p);
    const QString bad = dir_.filePath(QStringLiteral("broken.mp4"));
    QFile f(bad);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("this is not a video");
    f.close();
    pool.import({bad, video_});
    QVERIFY(pool.waitForIdle());
    int failed = 0, ok = 0;
    for (const Asset& a : p.assets()) {
      if (a.status == AssetStatus::Failed) {
        ++failed;
        QVERIFY(a.error.has_value());
      }
      if (a.status == AssetStatus::Analyzed) ++ok;
    }
    QCOMPARE(failed, 1);
    QCOMPARE(ok, 1);
  }

  void missingMediaIsFlaggedAndRecovers() {
    const QString copy = dir_.filePath(QStringLiteral("copy.wav"));
    QVERIFY(QFile::copy(audio_, copy));
    Project p;
    MediaPool pool(p);
    pool.import({copy});
    QVERIFY(pool.waitForIdle());
    QCOMPARE(p.assets()[0].status, AssetStatus::Analyzed);
    QVERIFY(QFile::remove(copy));
    pool.refreshAvailability();
    QCOMPARE(p.assets()[0].status, AssetStatus::Missing);
    QVERIFY(pool.data(pool.index(0), MediaPool::MissingRole).toBool());
    QVERIFY(QFile::copy(audio_, copy));
    pool.refreshAvailability();
    QVERIFY(pool.waitForIdle());
    QCOMPARE(p.assets()[0].status, AssetStatus::Analyzed);
  }

  void filtersByNameAndKind() {
    Project p;
    MediaPool pool(p);
    pool.import({video_, audio_, image_});
    QVERIFY(pool.waitForIdle());
    pool.setFilterKind(QStringLiteral("audio"));
    QCOMPARE(pool.rowCount(), 1);
    pool.setFilterKind(QStringLiteral("all"));
    pool.setFilterText(QStringLiteral("CLIP"));
    QCOMPARE(pool.rowCount(), 1);
    QCOMPARE(pool.data(pool.index(0), MediaPool::NameRole).toString(), QStringLiteral("clip.mp4"));
    pool.setFilterText(QStringLiteral("zzz"));
    QCOMPARE(pool.rowCount(), 0);
  }

  void removeAssetWithClipsIsUndoable() {
    Project p;
    MediaPool pool(p);
    pool.import({video_});
    QVERIFY(pool.waitForIdle());
    const QString id = p.assets()[0].id;
    ItemInit init;
    init.id = QStringLiteral("itm_x");
    init.trackId = trackNamed(p.doc(), TrackKind::Video).id;
    init.startFrame = 0;
    init.durationFrames = 30;
    init.assetId = id;
    init.sourceInFrame = 0;
    QVERIFY(p.apply(ItemAdd{createItem(ItemType::Video, init), false}));
    QCOMPARE(pool.usesOf(id), 1);
    QVERIFY(!pool.removeAsset(id, false)); // asks for confirmation first
    QVERIFY(pool.removeAsset(id, true));
    QVERIFY(p.assets().empty());
    QVERIFY(p.doc().items.empty());
    QCOMPARE(pool.rowCount(), 0);
  }

  void reopenedProjectRebuildsPreviews() {
    QTemporaryDir out;
    const QString file = out.filePath(QStringLiteral("p.json"));
    {
      Project p;
      MediaPool pool(p);
      pool.import({video_});
      QVERIFY(pool.waitForIdle());
      QVERIFY(p.saveAs(file));
    }
    Project q;
    MediaPool pool(q);
    QVERIFY(q.open(file));
    pool.projectLoaded();
    QVERIFY(pool.waitForIdle());
    QVERIFY(pool.preview(q.assets()[0].id));
    QCOMPARE(q.assets()[0].status, AssetStatus::Analyzed);
  }

  void reopenedProjectWithMissingFileMarksIt() {
    QTemporaryDir out;
    const QString file = out.filePath(QStringLiteral("p.json"));
    Project p;
    p.addAsset(makeAsset(QStringLiteral("ast_gone"), AssetKind::Video, QStringLiteral("gone.mp4"), 1000, out.filePath(QStringLiteral("gone.mp4"))));
    QVERIFY(p.saveAs(file));
    Project q;
    MediaPool pool(q);
    QVERIFY(q.open(file));
    pool.projectLoaded();
    QCOMPARE(q.assets()[0].status, AssetStatus::Missing);
    QVERIFY(pool.waitForIdle());
  }
};

} // namespace tst
using tst::TstMediaPool;
QTEST_GUILESS_MAIN(TstMediaPool)
#include "tst_media_pool.moc"
