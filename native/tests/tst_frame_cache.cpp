#include "media/frame_cache.h"
#include "media/frame_service.h"
#include "media_testutil.h"

#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>

using namespace sf;
using sf::test::readIndex;

namespace {

VideoFramePtr makeFrame(qint64 index, int w = 100, int h = 100) {
  auto f = std::make_shared<VideoFrame>();
  f->index = index;
  f->image = QImage(w, h, QImage::Format_RGBA8888);
  f->image.fill(Qt::black);
  return f;
}

constexpr qint64 kFrameBytes = 100 * 100 * 4;

} // namespace

class TstFrameCache : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QString clip;    // 320x240, 100 frames, IDR every 30, no B-frames
  QString clipB;   // same with B-frames

  static constexpr qint64 kClipFrameBytes = 320 * 240 * 4;

  // Waits (pumping events) until every listed frame is in the cache.
  bool waitCached(FrameService& svc, AssetId id, const QList<qint64>& frames, int timeoutMs = 8000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
      bool all = true;
      for (const qint64 n : frames) all = all && svc.cache().contains(id, n);
      if (all) return true;
      QTest::qWait(5);
    }
    return false;
  }

  void verifyCached(FrameService& svc, AssetId id, const QList<qint64>& frames) {
    for (const qint64 n : frames) {
      const VideoFramePtr f = svc.cached(id, n);
      QVERIFY2(f, qPrintable(QStringLiteral("frame %1 not cached").arg(n)));
      QCOMPARE(f->index, n);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
    }
  }

  FrameService::Options swOptions(qint64 cacheBytes = 256ll << 20) {
    FrameService::Options o;
    o.hw = HwMode::Off;
    o.cacheBytes = cacheBytes;
    return o;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir.isValid());
    QVERIFY2(!sf::test::ffmpegExe().isEmpty(), "SF_FFMPEG_EXE not set");
    clip = dir.filePath(QStringLiteral("a.mp4"));
    QString log;
    QVERIFY2(sf::test::makeIndexClip(clip, {"-c:v", "libopenh264", "-g", "30"}, 320, 240, 25, 4, {}, &log), qPrintable(log));
    clipB = dir.filePath(QStringLiteral("b.mp4"));
    QVERIFY2(sf::test::makeIndexClip(clipB, {"-c:v", "mpeg4", "-g", "30", "-bf", "2", "-q:v", "2"}, 320, 240, 25, 4, {}, &log), qPrintable(log));
  }

  // ---- FrameCache ----

  void evictsLeastRecentlyUsedFirst() {
    FrameCache c(3 * kFrameBytes);
    c.put(1, makeFrame(0));
    c.put(1, makeFrame(1));
    c.put(1, makeFrame(2));
    QVERIFY(c.get(1, 0)); // frame 0 is now the freshest, frame 1 the stalest
    c.put(1, makeFrame(3));
    QVERIFY(c.contains(1, 0));
    QVERIFY(!c.contains(1, 1));
    QVERIFY(c.contains(1, 2));
    QVERIFY(c.contains(1, 3));
    QCOMPARE(c.stats().evictions, 1);
    QCOMPARE(c.count(), 3);
  }

  void neverExceedsTheByteLimit() {
    const qint64 limit = 1'000'000;
    FrameCache c(limit);
    QRandomGenerator rng(7);
    for (int i = 0; i < 2000; ++i) {
      const int w = 10 + rng.bounded(150);
      const int h = 10 + rng.bounded(150);
      c.put(rng.bounded(3), makeFrame(rng.bounded(60), w, h));
      QVERIFY2(c.bytes() <= limit, qPrintable(QString::number(c.bytes())));
      if (rng.bounded(4) == 0) c.get(rng.bounded(3), rng.bounded(60));
    }
    QVERIFY(c.stats().evictions > 100);
    // bytes() is the real sum of what is held
    qint64 sum = 0;
    for (quint64 a = 0; a < 3; ++a) {
      for (qint64 n = 0; n < 60; ++n) {
        if (c.contains(a, n)) sum += c.get(a, n)->byteSize();
      }
    }
    QCOMPARE(sum, c.bytes());
  }

  void shrinkingTheLimitEvictsImmediately() {
    FrameCache c(10 * kFrameBytes);
    for (int i = 0; i < 10; ++i) c.put(1, makeFrame(i));
    c.setByteLimit(4 * kFrameBytes);
    QCOMPARE(c.count(), 4);
    QVERIFY(c.contains(1, 9));
    QVERIFY(!c.contains(1, 0));
  }

  void oversizedFramesAreNotKept() {
    FrameCache c(kFrameBytes / 2);
    c.put(1, makeFrame(0));
    QCOMPARE(c.count(), 0);
    QCOMPARE(c.bytes(), 0);
  }

  void reinsertingDoesNotDoubleCount() {
    FrameCache c(10 * kFrameBytes);
    c.put(1, makeFrame(5));
    c.put(1, makeFrame(5));
    c.put(1, makeFrame(5, 50, 50)); // replaced by a smaller one
    QCOMPARE(c.count(), 1);
    QCOMPARE(c.bytes(), 50 * 50 * 4);
  }

  void evictedFramesStayValidForHolders() {
    FrameCache c(kFrameBytes);
    c.put(1, makeFrame(0));
    const VideoFramePtr held = c.get(1, 0);
    c.put(1, makeFrame(1)); // evicts 0
    QVERIFY(!c.contains(1, 0));
    QVERIFY(held);
    QCOMPARE(held->image.size(), QSize(100, 100)); // still readable
    QCOMPARE(held->image.pixelColor(5, 5), QColor(Qt::black));
  }

  void clearAssetOnlyRemovesThatAsset() {
    FrameCache c(100 * kFrameBytes);
    for (int i = 0; i < 5; ++i) {
      c.put(1, makeFrame(i));
      c.put(2, makeFrame(i));
    }
    c.clearAsset(1);
    QCOMPARE(c.count(), 5);
    QCOMPARE(c.bytes(), 5 * kFrameBytes);
    QVERIFY(c.contains(2, 3));
    QVERIFY(!c.contains(1, 3));
  }

  void countsHitsAndMisses() {
    FrameCache c(10 * kFrameBytes);
    c.put(1, makeFrame(0));
    QVERIFY(c.get(1, 0));
    QVERIFY(!c.get(1, 1));
    QVERIFY(c.contains(1, 0)); // contains() is not a lookup
    QCOMPARE(c.stats().hits, 1);
    QCOMPARE(c.stats().misses, 1);
  }

  void survivesConcurrentUse() {
    const qint64 limit = 20 * kFrameBytes;
    FrameCache c(limit);
    std::atomic<bool> bad{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
      threads.emplace_back([&, t] {
        QRandomGenerator rng(100 + t);
        for (int i = 0; i < 4000; ++i) {
          const AssetId a = rng.bounded(3);
          const qint64 n = rng.bounded(50);
          if (rng.bounded(2) == 0) {
            c.put(a, makeFrame(n));
          } else if (const VideoFramePtr f = c.get(a, n); f && f->index != n) {
            bad = true;
          }
          if (c.bytes() > limit) bad = true;
          if (i % 997 == 0) c.clearAsset(a);
        }
      });
    }
    for (auto& th : threads) th.join();
    QVERIFY(!bad);
    QVERIFY(c.bytes() <= limit);
  }

  // ---- FrameService ----

  void opensAssetsInTheBackground() {
    FrameService svc(swOptions());
    QSignalSpy opened(&svc, &FrameService::assetOpened);
    const AssetId id = svc.openAsset(clip);
    QVERIFY(id != 0);
    QVERIFY(opened.wait(8000));
    const auto info = svc.info(id);
    QVERIFY(info);
    QCOMPARE(info->frameCount, 100);
    QCOMPARE(info->width, 320);
    QVERIFY(svc.decoderName(id).contains(QLatin1String("software")));
  }

  void failedOpenIsReported() {
    FrameService svc(swOptions());
    QSignalSpy failed(&svc, &FrameService::assetFailed);
    const AssetId id = svc.openAsset(dir.filePath(QStringLiteral("missing.mp4")));
    QVERIFY(failed.wait(8000));
    QVERIFY(!failed.first().at(1).toString().isEmpty());
    QVERIFY(!svc.frameBlocking(id, 0));
    QVERIFY(!svc.info(id));
  }

  void requestedFrameArrivesCorrectly() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clip);
    QSignalSpy ready(&svc, &FrameService::frameReady);
    svc.requestFrame(id, 42);
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 8000);
    QCOMPARE(ready.last().at(0).toULongLong(), id);
    QCOMPARE(ready.last().at(1).toLongLong(), 42);
    verifyCached(svc, id, {42});
    // already cached: answered at once without decoding
    ready.clear();
    svc.requestFrame(id, 42);
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 1000);
  }

  // A burst of scrub positions: only the newest has to arrive, and it must be right.
  void scrubbingLatestWins() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clipB);
    QSignalSpy ready(&svc, &FrameService::frameReady);
    QRandomGenerator rng(3);
    for (int i = 0; i < 40; ++i) svc.requestFrame(id, rng.bounded(100u));
    svc.requestFrame(id, 77);
    QTRY_VERIFY_WITH_TIMEOUT(svc.cache().contains(id, 77), 8000);
    verifyCached(svc, id, {77});
    // the service didn't have to produce all 41 targets
    QVERIFY(ready.size() <= 41);
    // and it is idle afterwards: nothing else arrives
    const int n = svc.cache().count();
    QTest::qWait(200);
    QCOMPARE(svc.cache().count(), n);
  }

  // Seeking to frame 70 walks the GOP from the keyframe at 60; the frames just before the target
  // come along for free so stepping backwards is a cache hit.
  void scrubbingBackStepsAreCached() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clip);
    svc.requestFrame(id, 70);
    QTRY_VERIFY_WITH_TIMEOUT(svc.cache().contains(id, 70), 8000);
    verifyCached(svc, id, {62, 63, 64, 65, 66, 67, 68, 69, 70});
    QVERIFY(!svc.cache().contains(id, 61)); // keepBehind = 8, not the whole GOP
  }

  void forwardPlaybackReadsAhead() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clipB);
    svc.setPlayhead(id, 10, +1);
    QVERIFY(waitCached(svc, id, {11, 12, 15, 20, 22}));
    verifyCached(svc, id, {11, 12, 15, 20, 22});
    QVERIFY(!svc.cache().contains(id, 40)); // bounded read-ahead
    // the playhead moves on and the window follows without anything being re-decoded or lost
    for (qint64 p = 11; p < 60; ++p) {
      svc.setPlayhead(id, p, +1);
      QTest::qWait(8);
    }
    QVERIFY(waitCached(svc, id, {61, 62, 65, 70}));
    verifyCached(svc, id, {61, 62, 65, 70});
  }

  void reversePlaybackReadsBehind() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clipB);
    svc.setPlayhead(id, 80, -1);
    QVERIFY(waitCached(svc, id, {79, 78, 77, 70, 68}));
    verifyCached(svc, id, {79, 78, 77, 70, 68});
    svc.setPlayhead(id, 5, -1); // near the start: must stop at frame 0, not wander
    QVERIFY(waitCached(svc, id, {4, 3, 2, 1, 0}));
    verifyCached(svc, id, {0, 1, 2, 3, 4});
  }

  void stoppedPlaybackStopsReadingAhead() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clip);
    svc.setPlayhead(id, 0, +1);
    QVERIFY(waitCached(svc, id, {1, 12}));
    svc.setPlayhead(id, 50, 0);
    QTest::qWait(150);
    const qint64 n = svc.cache().count();
    QTest::qWait(150);
    QCOMPARE(svc.cache().count(), n);
    QVERIFY(!svc.cache().contains(id, 51));
  }

  void blockingReadsReturnExactFrames() {
    FrameService svc(swOptions());
    const AssetId id = svc.openAsset(clipB);
    for (const qint64 n : {0, 99, 50, 49, 51, 12, 12, 88}) {
      const VideoFramePtr f = svc.frameBlocking(id, n);
      QVERIFY(f);
      QCOMPARE(f->index, n);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
    }
  }

  void blockingReadsFromManyThreads() {
    FrameService svc(swOptions());
    const AssetId a = svc.openAsset(clip);
    const AssetId b = svc.openAsset(clipB);
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
      threads.emplace_back([&, t] {
        QRandomGenerator rng(40 + t);
        const AssetId asset = t % 2 ? a : b;
        for (int i = 0; i < 25; ++i) {
          const qint64 n = rng.bounded(100u);
          const VideoFramePtr f = svc.frameBlocking(asset, n);
          if (!f || f->index != n || readIndex(f->image) != n) ++wrong;
        }
      });
    }
    for (auto& th : threads) th.join();
    QCOMPARE(wrong.load(), 0);
  }

  // The cache bound holds with decoders feeding it, and frames come out right even when the cache
  // can't hold them (smaller than one frame).
  void cacheStaysBoundedUnderLoad() {
    const qint64 limit = 12 * kClipFrameBytes;
    FrameService svc(swOptions(limit));
    const AssetId id = svc.openAsset(clip);
    svc.setPlayhead(id, 0, +1);
    for (qint64 n = 0; n < 100; ++n) {
      const VideoFramePtr f = svc.frameBlocking(id, n);
      QVERIFY(f);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
      QVERIFY2(svc.cache().bytes() <= limit, qPrintable(QString::number(svc.cache().bytes())));
      svc.setPlayhead(id, n, +1);
    }
    QVERIFY(svc.cache().stats().evictions > 50);

    FrameService tiny(swOptions(kClipFrameBytes / 2));
    const AssetId t = tiny.openAsset(clip);
    for (const qint64 n : {5, 6, 90, 3}) {
      const VideoFramePtr f = tiny.frameBlocking(t, n);
      QVERIFY(f);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
    }
    QCOMPARE(tiny.cache().bytes(), 0);
  }

  void severalAssetsDecodeIndependently() {
    FrameService svc(swOptions());
    QList<AssetId> ids;
    for (int i = 0; i < 4; ++i) ids << svc.openAsset(i % 2 ? clip : clipB);
    for (const AssetId id : ids) svc.setPlayhead(id, 20 + int(id), +1);
    for (const AssetId id : ids) svc.requestFrame(id, 90 - int(id));
    for (const AssetId id : ids) {
      QVERIFY(waitCached(svc, id, {90 - qint64(id), 21 + qint64(id)}));
      verifyCached(svc, id, {90 - qint64(id), 21 + qint64(id)});
    }
    svc.closeAsset(ids[0]);
    QVERIFY(!svc.cached(ids[0], 90 - qint64(ids[0])));
    QVERIFY(svc.cached(ids[1], 90 - qint64(ids[1])));
  }

  void closingAndDestroyingWhileBusyIsSafe() {
    for (int round = 0; round < 5; ++round) {
      FrameService svc(swOptions());
      const AssetId a = svc.openAsset(clipB);
      const AssetId b = svc.openAsset(clip);
      svc.setPlayhead(a, 0, +1);
      svc.setPlayhead(b, 99, -1);
      for (int i = 0; i < 20; ++i) {
        svc.requestFrame(a, (i * 37) % 100);
        svc.requestFrame(b, (i * 53) % 100);
      }
      std::thread blocker([&] { svc.frameBlocking(a, 99); }); // released with null when the asset closes (or decoded)
      svc.closeAsset(a);
      blocker.join();
      // `svc` is destroyed with b still working
    }
    QVERIFY(true);
  }

  void hardwareDecodingThroughTheService() {
    const QString hwClip = dir.filePath(QStringLiteral("hw.mp4"));
    QString log;
    if (!sf::test::makeIndexClip(hwClip, {"-c:v", "libopenh264", "-g", "30"}, 640, 360, 25, 2, {}, &log)) QSKIP("cannot encode");
    FrameService::Options o;
    o.cacheBytes = 64ll << 20;
    o.hw = HwMode::Auto;
    FrameService svc(o);
    const AssetId id = svc.openAsset(hwClip);
    for (const qint64 n : {0, 25, 49, 10}) {
      const VideoFramePtr f = svc.frameBlocking(id, n);
      QVERIFY(f);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
    }
    qInfo() << "service decoder:" << svc.decoderName(id);
    if (!svc.decoderName(id).contains(QLatin1String("d3d11va"))) QSKIP("no D3D11VA on this machine; software path verified above");
    QVERIFY(svc.cached(id, 49)->hardware);
  }
};

QTEST_GUILESS_MAIN(TstFrameCache)
#include "tst_frame_cache.moc"
