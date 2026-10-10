// Proxies: file naming and cache location, the transcode (size, frame count, all-intra, timestamps), invalidation
// when the source changes, the media pool's background queue, preview mapping (Live providers use the proxy,
// Blocking ones never do) and that export reads the original even when an asset has a proxy.

#include "editor/media_pool.h"
#include "editor_testutil.h"
#include "export/exporter.h"
#include "export/proxy.h"
#include "media/probe.h"
#include "media/video_decoder.h"
#include "render/media_provider.h"
#include "render/offscreen.h"
#include "render_testutil.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QThread>

#include <atomic>

using namespace sf;
using namespace sf::xport;
using namespace sf::test;
using sf::editor::MediaPool;


namespace {

bool makeSolid(const QString& path, const QString& color, int w, int h, double seconds = 1) {
  return runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), solidGraph(color, w, h, 25, seconds), QStringLiteral("-c:v"),
                    QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-g"), QStringLiteral("25"), path});
}

bool makeIndex(const QString& path, int w, int h, double seconds) {
  return runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), indexSource(w, h, 25, seconds), QStringLiteral("-c:v"),
                    QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-g"), QStringLiteral("25"), QStringLiteral("-pix_fmt"),
                    QStringLiteral("yuv420p"), path});
}

} // namespace

class TstProxy : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString src_;

  QString p(const QString& name) { return dir_.filePath(name); }

private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(dir_.isValid());
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE is not set");
    src_ = p(QStringLiteral("src.mp4"));
    QVERIFY(makeIndex(src_, 320, 240, 2));
  }

  void namesAndCacheLocation() {
    const QString a = proxyFileName(src_);
    QCOMPARE(a, proxyFileName(src_)); // stable
    QVERIFY(a.endsWith(QStringLiteral(".mp4")));
    QVERIFY(a != proxyFileName(p(QStringLiteral("other.mp4"))));
    // saved project: a folder next to the file; unsaved: the user cache, never the Electron app's data
    const QString saved = proxyCacheDir(p(QStringLiteral("My Film.json")));
    QCOMPARE(saved, QDir(dir_.path()).absoluteFilePath(QStringLiteral("My Film.proxies")));
    const QString unsaved = proxyCacheDir({});
    QVERIFY2(!unsaved.contains(QStringLiteral("Cutboard"), Qt::CaseInsensitive), qPrintable(unsaved));
    QVERIFY(unsaved.endsWith(QStringLiteral("proxies")));
  }

  void transcodeKeepsFramesAndTimestamps() {
    const QString cache = p(QStringLiteral("cache1"));
    ProxyOptions o;
    o.width = 160;
    std::vector<double> seen;
    const ProxyResult r = makeProxy(src_, cache, o, [&](double f) { seen.push_back(f); });
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(!r.unneeded);
    QCOMPARE(r.path, proxyPathFor(src_, cache));
    QVERIFY(QFileInfo::exists(r.path));
    QVERIFY(!QFileInfo::exists(r.path + QStringLiteral(".part")));
    QCOMPARE(existingProxy(src_, cache), r.path);
    QVERIFY(!seen.empty());
    QCOMPARE(seen.back(), 1.0);

    QString err;
    const auto in = probeMedia(src_, &err, ProbeOptions{.vfrScanLimit = 0});
    const auto px = probeMedia(r.path, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY(in && px);
    QCOMPARE(px->width, 160);
    QCOMPARE(px->height, 120);
    QCOMPARE(px->videoCodec, QStringLiteral("h264"));
    QCOMPARE(px->frameCount, in->frameCount);
    QVERIFY(!px->hasAudio);
    QVERIFY(std::abs(px->videoStartSec - in->videoStartSec) < 0.001);
    QVERIFY(std::abs(px->durationMs - in->durationMs) <= 40);

    // every frame is a keyframe, and frame N of the proxy is frame N of the source
    VideoOpenOptions vo;
    vo.hw = HwMode::Off;
    auto d = VideoDecoder::open(r.path, vo, &err);
    QVERIFY2(d, qPrintable(err));
    for (qint64 i = 0; i < d->frameCount(); ++i) QVERIFY2(d->isKeyFrame(i), qPrintable(QString::number(i)));
    for (const qint64 i : {qint64(0), qint64(13), qint64(49)}) {
      const auto f = d->frameAt(i);
      QVERIFY(f);
      QCOMPARE(readIndex(f->image), static_cast<int>(i));
    }
  }

  void smallSourcesNeedNone() {
    ProxyOptions o; // 960 wide: the 320 px clip is already small
    const ProxyResult r = makeProxy(src_, p(QStringLiteral("cache2")), o);
    QVERIFY(r.ok);
    QVERIFY(r.unneeded);
    QVERIFY(existingProxy(src_, p(QStringLiteral("cache2"))).isEmpty());
  }

  void cancelLeavesNothing() {
    const QString cache = p(QStringLiteral("cache3"));
    ProxyOptions o;
    o.width = 160;
    std::atomic<bool> cancel{true};
    const ProxyResult r = makeProxy(src_, cache, o, {}, &cancel);
    QVERIFY(!r.ok);
    QVERIFY(r.cancelled);
    QVERIFY(!QFileInfo::exists(proxyPathFor(src_, cache)));
    QVERIFY(!QFileInfo::exists(proxyPathFor(src_, cache) + QStringLiteral(".part")));
  }

  void changedSourceInvalidates() {
    const QString src = p(QStringLiteral("changing.mp4"));
    QVERIFY(makeIndex(src, 320, 240, 1));
    const QString cache = p(QStringLiteral("cache4"));
    ProxyOptions o;
    o.width = 160;
    const ProxyResult r = makeProxy(src, cache, o);
    QVERIFY2(r.ok, qPrintable(r.error));
    const QString before = r.path;
    QVERIFY(!existingProxy(src, cache).isEmpty());
    // replaced by a different file at the same path (different size and time)
    QThread::msleep(20);
    QVERIFY(makeIndex(src, 320, 240, 1.5));
    QVERIFY(proxyPathFor(src, cache) != before);
    QVERIFY2(existingProxy(src, cache).isEmpty(), "a proxy of the old file must not apply to the new one");
    QVERIFY(QFileInfo::exists(before)); // still on disk until the new one replaces it
    const ProxyResult r2 = makeProxy(src, cache, o);
    QVERIFY2(r2.ok, qPrintable(r2.error));
    QVERIFY(!QFileInfo::exists(before));
    QCOMPARE(existingProxy(src, cache), r2.path);
    // touching only the modification time is enough too
    QFile f(src);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    f.close();
    QVERIFY(existingProxy(src, cache).isEmpty());
  }

  // ---------------------------------------------------------------- media pool queue
  void poolMakesProxiesInTheBackground() {
    const QString video = p(QStringLiteral("pool.mp4"));
    QVERIFY(makeIndex(video, 320, 240, 1));
    sf::editor::Project proj;
    MediaPool pool(proj);
    pool.setProxyDirOverride(p(QStringLiteral("poolcache")));
    ProxyOptions o;
    o.width = 160;
    pool.setProxyOptions(o);
    const auto summary = pool.import({video});
    QCOMPARE(summary.added.size(), 1);
    QVERIFY(pool.waitForIdle());
    const QString id = summary.added[0];
    QCOMPARE(pool.proxyState(id), QStringLiteral("none"));
    QVERIFY(pool.proxyPath(id).isEmpty());

    QSignalSpy changed(&pool, &MediaPool::proxiesChanged);
    pool.setProxiesEnabled(true);
    QCOMPARE(pool.proxyPending(), 1);
    QVERIFY(pool.proxyState(id) == QLatin1String("queued") || pool.proxyState(id) == QLatin1String("running"));
    QVERIFY(pool.waitForProxies());
    QCOMPARE(pool.proxyState(id), QStringLiteral("ready"));
    const QString proxy = pool.proxyPath(id);
    QVERIFY(QFileInfo::exists(proxy));
    QVERIFY(changed.count() >= 2);
    QCOMPARE(pool.data(pool.index(0), MediaPool::ProxyStateRole).toString(), QStringLiteral("ready"));

    // the source is replaced: the old proxy no longer applies and a new one is made
    QThread::msleep(20);
    QVERIFY(makeIndex(video, 320, 240, 1.4));
    pool.refreshAvailability();
    QVERIFY(pool.proxyPath(id).isEmpty() || pool.proxyPath(id) != proxy);
    QVERIFY(pool.waitForProxies());
    QCOMPARE(pool.proxyState(id), QStringLiteral("ready"));
    QVERIFY(pool.proxyPath(id) != proxy);
    QVERIFY(!QFileInfo::exists(proxy));

    // cancelled before it starts: nothing is left
    pool.setProxiesEnabled(false);
    const QString video2 = p(QStringLiteral("pool2.mp4"));
    QVERIFY(makeIndex(video2, 320, 240, 1));
    const auto s2 = pool.import({video2});
    QVERIFY(pool.waitForIdle());
    pool.cancelProxies();
    pool.requestProxy(s2.added[0]);
    QVERIFY(pool.waitForProxies());
    QVERIFY(pool.proxyState(s2.added[0]) == QLatin1String("ready")); // requested after the cancel: runs
  }

  // ---------------------------------------------------------------- preview mapping
  void previewUsesProxyExportDoesNot() {
    const QString orig = p(QStringLiteral("orig_blue.mp4")), prox = p(QStringLiteral("proxy_red.mp4"));
    QVERIFY(makeSolid(orig, QStringLiteral("0x0000ff"), 320, 180, 1));
    QVERIFY(makeSolid(prox, QStringLiteral("0xff0000"), 160, 90, 1));

    render::AssetTable table;
    table["ast_a"] = {orig, AssetKind::Video, prox};
    TimelineDoc doc = newDoc(320, 180, 25);
    addMedia(doc, ItemType::Video, mainTrack(doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, 25);
    const Item& item = doc.items.front();

    FrameService::Options fo;
    fo.hw = HwMode::Off;
    {
      FrameService svc(fo);
      render::FrameServiceProvider live(svc, table, render::FrameServiceProvider::Mode::Live);
      auto sizeOf = [&] {
        for (int i = 0; i < 2000; ++i) {
          const render::Visual v = live.visual(item, 0, 25);
          if (v.state == render::Visual::State::Ready) return v.size;
          QThread::msleep(5);
        }
        return QSize();
      };
      QCOMPARE(sizeOf(), QSize(320, 180)); // proxies off: the original
      live.setUseProxies(true);
      QCOMPARE(sizeOf(), QSize(160, 90)); // on: the proxy
      live.setUseProxies(false);
      QCOMPARE(sizeOf(), QSize(320, 180));
      // an asset without a (readable) proxy falls back to the original
      table["ast_a"].proxyPath = p(QStringLiteral("missing_proxy.mp4"));
      live.setAssets(table);
      live.setUseProxies(true);
      QCOMPARE(sizeOf(), QSize(320, 180));
      table["ast_a"].proxyPath = prox;
    }
    {
      // Blocking providers (export, tests) ignore proxies even when asked to use them
      FrameService svc(fo);
      render::FrameServiceProvider blocking(svc, table, render::FrameServiceProvider::Mode::Blocking);
      blocking.setUseProxies(true);
      const render::Visual v = blocking.visual(item, 0, 25);
      QCOMPARE(v.state, render::Visual::State::Ready);
      QCOMPARE(v.size, QSize(320, 180));
    }

    // and the exporter reads the original: the picture is blue, not the proxy's red
    QString err;
    if (!render::OffscreenRenderer::create(&err)) QSKIP(qPrintable(QStringLiteral("no D3D11 device: %1").arg(err)));
    ExportSettings st;
    st.outputPath = p(QStringLiteral("proxy_export.mp4"));
    st.overwrite = true;
    st.audio = AudioCodec::None;
    st.encoderPreset = QStringLiteral("ultrafast");
    const ExportResult r = runExport(doc, table, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    VideoOpenOptions vo;
    vo.hw = HwMode::Off;
    auto d = VideoDecoder::open(st.outputPath, vo, &err);
    QVERIFY2(d, qPrintable(err));
    const auto f = d->frameAt(5);
    QVERIFY(f);
    const QRgb px = f->image.pixel(160, 90);
    QVERIFY2(qBlue(px) > 200 && qRed(px) < 60, qPrintable(describe(px)));
  }
};

QTEST_MAIN(TstProxy)
#include "tst_proxy.moc"
