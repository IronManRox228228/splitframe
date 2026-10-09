#include "media/video_decoder.h"
#include "media/probe.h"
#include "media_testutil.h"

#include <QHash>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using sf::test::readIndex;

namespace {

struct CodecCase {
  QString name;
  QStringList args;
  QString ext;
  int w = 320, h = 240;
  double seconds = 4; // 25 fps -> 100 frames
  bool hwCapable = false;
  QString pixFmt = QStringLiteral("yuv420p");
};

QList<CodecCase> codecCases() {
  const QStringList g30{QStringLiteral("-g"), QStringLiteral("30")};
  return {
      {QStringLiteral("h264_sw"), QStringList{"-c:v", "libopenh264", "-g", "30"}, QStringLiteral("mp4"), 320, 240, 4, true},
      {QStringLiteral("h264_bframes"), QStringList{"-c:v", "h264_nvenc", "-g", "30", "-bf", "2", "-b:v", "2M"}, QStringLiteral("mp4"), 320, 240, 4, true},
      {QStringLiteral("hevc_bframes"), QStringList{"-c:v", "hevc_nvenc", "-g", "30", "-bf", "2", "-b:v", "2M"}, QStringLiteral("mp4"), 320, 240, 4, true},
      {QStringLiteral("hevc_10bit"), QStringList{"-c:v", "hevc_nvenc", "-profile:v", "main10", "-g", "30", "-bf", "2", "-b:v", "2M"},
       QStringLiteral("mp4"), 320, 240, 4, true, QStringLiteral("p010le")},
      {QStringLiteral("mpeg4_bframes"), QStringList{"-c:v", "mpeg4", "-g", "30", "-bf", "2", "-q:v", "2"}, QStringLiteral("mp4")},
      {QStringLiteral("mpeg2_ps"), QStringList{"-c:v", "mpeg2video", "-g", "12", "-bf", "2", "-q:v", "2"}, QStringLiteral("mpg")},
      {QStringLiteral("prores"), QStringList{"-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"}, QStringLiteral("mov")},
      {QStringLiteral("dnxhd"), QStringList{"-c:v", "dnxhd", "-b:v", "90M", "-pix_fmt", "yuv422p"}, QStringLiteral("mov"), 1280, 720, 2},
      {QStringLiteral("vp9"), QStringList{"-c:v", "libvpx-vp9", "-g", "30", "-b:v", "2M", "-deadline", "realtime"}, QStringLiteral("webm")},
      {QStringLiteral("av1"), QStringList{"-c:v", "libsvtav1", "-g", "30"}, QStringLiteral("mkv")},
      {QStringLiteral("mjpeg"), QStringList{"-c:v", "mjpeg", "-q:v", "3"}, QStringLiteral("avi")},
      {QStringLiteral("h264_mkv"), QStringList{"-c:v", "libopenh264", "-g", "30"}, QStringLiteral("mkv")},
  };
}

} // namespace

class TstVideoDecoder : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QHash<QString, QString> clips;
  QString skipReason;

  // Builds (once) the clip for a case. Empty path + skipReason set when the encoder is unavailable.
  QString clipFor(const CodecCase& c) {
    if (clips.contains(c.name)) return clips.value(c.name);
    const QString path = dir.filePath(c.name + QLatin1Char('.') + c.ext);
    QString log;
    const bool ok = sf::test::makeIndexClip(path, c.args, c.w, c.h, 25, c.seconds, {}, &log, c.pixFmt);
    clips.insert(c.name, ok ? path : QString());
    if (!ok) skipReason = QStringLiteral("cannot encode %1: %2").arg(c.name, log.left(200));
    return clips.value(c.name);
  }

  static CodecCase caseNamed(const QString& name) {
    for (const CodecCase& c : codecCases()) {
      if (c.name == name) return c;
    }
    return {};
  }

  void checkFrame(VideoDecoder& dec, qint64 n, const char* what) {
    const VideoFramePtr f = dec.frameAt(n);
    QVERIFY2(f, qPrintable(QStringLiteral("%1: frameAt(%2) failed: %3").arg(QLatin1String(what)).arg(n).arg(dec.lastError())));
    QCOMPARE(f->index, n);
    const int painted = readIndex(f->image);
    QVERIFY2(painted == n, qPrintable(QStringLiteral("%1: asked for frame %2, got picture of frame %3").arg(QLatin1String(what)).arg(n).arg(painted)));
  }

private slots:
  void initTestCase() {
    QVERIFY(dir.isValid());
    QVERIFY2(!sf::test::ffmpegExe().isEmpty(), "SF_FFMPEG_EXE not set");
  }

  void frameAccurateSeek_data() {
    QTest::addColumn<QString>("codec");
    for (const CodecCase& c : codecCases()) QTest::newRow(qPrintable(c.name)) << c.name;
  }

  // The core promise: decode(N) is the frame painted "N", sequentially, by random access, and when
  // walking backwards, for GOPs with and without B-frames.
  void frameAccurateSeek() {
    QFETCH(QString, codec);
    const CodecCase c = caseNamed(codec);
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));

    QString err;
    VideoOpenOptions opt;
    opt.hw = HwMode::Off;
    auto dec = VideoDecoder::open(clip, opt, &err);
    QVERIFY2(dec, qPrintable(err));
    const qint64 total = qRound(c.seconds * 25);
    QCOMPARE(dec->frameCount(), total);
    QVERIFY(!dec->usingHardware());

    // sequential playback
    for (qint64 n = 0; n < total; ++n) {
      const VideoFramePtr f = dec->next();
      QVERIFY2(f, qPrintable(dec->lastError()));
      QCOMPARE(f->index, n);
      QCOMPARE(readIndex(f->image), static_cast<int>(n));
    }
    QVERIFY(!dec->next()); // past the end

    // jumps around keyframe boundaries, to both ends, and repeated frames
    const QList<qint64> jumps{0, total - 1, total / 2, 29, 30, 31, 59, 60, 61, 7, total - 2, 1, 45, 44, 43, 42, 10, 0, total - 1, 0};
    for (const qint64 n : jumps) {
      if (n < total) checkFrame(*dec, n, "jump");
    }
    // walking backwards frame by frame (what scrubbing left does)
    for (qint64 n = total - 10; n >= total - 50; --n) checkFrame(*dec, n, "backward");
    // forward in small steps and strides
    for (qint64 n = 3; n < total; n += 3) checkFrame(*dec, n, "stride3");
    // random access
    QRandomGenerator rng(1234);
    for (int i = 0; i < 40; ++i) checkFrame(*dec, rng.bounded(static_cast<quint32>(total)), "random");
  }

  void outOfRangeFails() {
    const CodecCase c = caseNamed(QStringLiteral("mpeg4_bframes"));
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    QVERIFY(!dec->frameAt(-1));
    QVERIFY(!dec->frameAt(dec->frameCount()));
    checkFrame(*dec, 5, "after failures");
  }

  void reusesDecoderState() {
    const CodecCase c = caseNamed(QStringLiteral("h264_sw")); // IDR every 30 frames
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    for (int i = 0; i < 100; ++i) QVERIFY(dec->next());
    QCOMPARE(dec->stats().seeks, 1); // the very first positioning, then pure sequential decode

    checkFrame(*dec, 10, "seek back"); // backwards needs a seek
    QCOMPARE(dec->stats().seeks, 2);
    const qint64 decodedBefore = dec->stats().framesDecoded;
    checkFrame(*dec, 13, "near forward"); // same GOP, a few ahead: continue, no seek
    QCOMPARE(dec->stats().seeks, 2);
    QCOMPARE(dec->stats().framesDecoded - decodedBefore, 3);
    checkFrame(*dec, 13, "repeat"); // already have it
    QCOMPARE(dec->stats().framesDecoded - decodedBefore, 3);
    checkFrame(*dec, 24, "same GOP");
    QCOMPARE(dec->stats().seeks, 2);
    checkFrame(*dec, 95, "far forward"); // several GOPs ahead: seeking to frame 90 is cheaper
    QCOMPARE(dec->stats().seeks, 3);
    QVERIFY(dec->stats().framesDecoded - decodedBefore < 3 + 11 + 12);
  }

  void cancelLeavesDecoderUsable() {
    const CodecCase c = caseNamed(QStringLiteral("h264_sw"));
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    QVERIFY(!dec->frameAt(80, [] { return true; }));
    int polls = 0;
    QVERIFY(!dec->frameAt(80, [&polls] { return ++polls > 5; }));
    checkFrame(*dec, 80, "after cancel");
    checkFrame(*dec, 81, "after cancel, next");
    checkFrame(*dec, 50, "after cancel, back");
  }

  void sinkReceivesFramesBeforeTarget() {
    const CodecCase c = caseNamed(QStringLiteral("hevc_bframes"));
    QString clip = clipFor(c);
    if (clip.isEmpty()) clip = clipFor(caseNamed(QStringLiteral("mpeg4_bframes")));
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    QList<qint64> seen;
    const VideoFramePtr f = dec->frameAt(75, {}, 70, [&](VideoFramePtr kept) {
      QCOMPARE(readIndex(kept->image), static_cast<int>(kept->index));
      seen << kept->index;
    });
    QVERIFY(f);
    QCOMPARE(seen, (QList<qint64>{70, 71, 72, 73, 74}));
  }

  void hardwareMatchesSoftware_data() {
    QTest::addColumn<QString>("codec");
    for (const CodecCase& c : codecCases()) {
      if (c.hwCapable) QTest::newRow(qPrintable(c.name)) << c.name;
    }
  }

  // D3D11VA must give the same frames, in the same order, with (nearly) the same pixels.
  void hardwareMatchesSoftware() {
    QFETCH(QString, codec);
    const CodecCase c = caseNamed(codec);
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    QString err;
    auto hw = VideoDecoder::open(clip, {HwMode::Required, 0}, &err);
    if (!hw) QSKIP(qPrintable(QStringLiteral("no D3D11VA for %1: %2").arg(codec, err)));
    auto sw = VideoDecoder::open(clip, {HwMode::Off, 0}, &err);
    QVERIFY2(sw, qPrintable(err));
    QCOMPARE(hw->frameCount(), sw->frameCount());

    double worst = 0;
    auto compare = [&](qint64 n) {
      const VideoFramePtr a = hw->frameAt(n);
      const VideoFramePtr b = sw->frameAt(n);
      QVERIFY2(a, qPrintable(hw->lastError()));
      QVERIFY2(b, qPrintable(sw->lastError()));
      QVERIFY(a->hardware);
      QVERIFY(!b->hardware);
      QCOMPARE(readIndex(a->image), static_cast<int>(n));
      QCOMPARE(readIndex(b->image), static_cast<int>(n));
      QCOMPARE(a->image.size(), b->image.size());
      qint64 sum = 0;
      for (int y = 0; y < a->image.height(); y += 3) {
        const uchar* pa = a->image.constScanLine(y);
        const uchar* pb = b->image.constScanLine(y);
        for (int x = 0; x < a->image.width() * 4; ++x) sum += std::abs(pa[x] - pb[x]);
      }
      const double mean = double(sum) / (double((a->image.height() + 2) / 3) * a->image.width() * 4);
      worst = std::max(worst, mean);
    };
    for (qint64 n = 0; n < hw->frameCount(); n += 1) compare(n);
    for (qint64 n : {99, 5, 64, 33, 98, 0, 31}) compare(n);
    QVERIFY2(worst < 2.0, qPrintable(QStringLiteral("mean abs diff %1").arg(worst)));
    QVERIFY(hw->usingHardware());
    QVERIFY(hw->decoderName().contains(QLatin1String("d3d11va")));
    qInfo() << codec << "hw:" << hw->decoderName() << "mean abs diff vs software" << worst;
  }

  void unsupportedCodecFallsBackToSoftware() {
    const CodecCase c = caseNamed(QStringLiteral("prores"));
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    QString err;
    QVERIFY(!VideoDecoder::open(clip, {HwMode::Required, 0}, &err)); // says so instead of silently degrading
    QVERIFY(!err.isEmpty());
    auto dec = VideoDecoder::open(clip, {HwMode::Auto, 0}, &err);
    QVERIFY2(dec, qPrintable(err));
    checkFrame(*dec, 40, "auto fallback");
    QVERIFY(!dec->usingHardware());
    QVERIFY2(dec->decoderName().contains(QLatin1String("software")), qPrintable(dec->decoderName()));
  }

  // Streams the GPU can't take must fall back even when the decoder opened fine.
  void unsupportedProfileFallsBackAtRuntime() {
    // MPEG-2 4:2:2 is something D3D11VA never decodes, so Auto must quietly use software
    CodecCase c{QStringLiteral("mpeg2_422"), QStringList{"-c:v", "mpeg2video", "-pix_fmt", "yuv422p", "-q:v", "2"}, QStringLiteral("mpg")};
    c.pixFmt = QStringLiteral("yuv422p");
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Auto, 0});
    QVERIFY(dec);
    checkFrame(*dec, 20, "422 auto");
    QVERIFY(!dec->usingHardware());
  }

  void variableFrameRateIndexing_data() {
    QTest::addColumn<QString>("ext");
    QTest::newRow("mkv") << "mkv";
    QTest::newRow("mp4") << "mp4";
  }

  // 50 frames at 25 fps, then 25 frames at 8.3 fps. Frame N is still the Nth presented frame and its
  // time comes from the real timestamps.
  void variableFrameRateIndexing() {
    QFETCH(QString, ext);
    const QString path = dir.filePath(QStringLiteral("vfr.") + ext);
    QString log;
    QVERIFY2(sf::test::makeIndexClip(path, {"-c:v", "libvpx-vp9", "-deadline", "realtime", "-g", "20", "-fps_mode", "passthrough", "-enc_time_base", "1:1000"}, 320, 240, 25, 3,
                                    {"-vf", "setpts='if(lt(N,50),N,50+(N-50)*3)'"}, &log),
             qPrintable(log));
    auto dec = VideoDecoder::open(path, {HwMode::Off, 0});
    QVERIFY(dec);
    QCOMPARE(dec->frameCount(), 75);
    QVERIFY(dec->info().vfr);
    QVERIFY(qAbs(dec->timeOfFrame(49) - 1.96) < 0.002);
    QVERIFY(qAbs(dec->timeOfFrame(50) - 2.0) < 0.002);
    QVERIFY(qAbs(dec->timeOfFrame(60) - 3.2) < 0.002);
    QCOMPARE(dec->indexAtTime(1.99), 49);
    QCOMPARE(dec->indexAtTime(2.0), 50);
    QCOMPARE(dec->indexAtTime(2.55), 54); // frame 54 is at 2.48, 55 at 2.60
    QCOMPARE(dec->indexAtTime(100.0), 74);
    QCOMPARE(dec->indexAtTime(-5.0), 0);
    for (const qint64 n : {0, 49, 50, 51, 74, 20, 21, 60, 3}) checkFrame(*dec, n, "vfr");
    // a frame asked for by time is the frame painted for that time
    const VideoFramePtr f = dec->frameAt(dec->indexAtTime(2.25));
    QVERIFY(f);
    QCOMPARE(readIndex(f->image), 52); // 2.0 + 2 * 0.12 = 2.24 <= 2.25 < 2.36

    const auto probe = probeMedia(path);
    QVERIFY(probe);
    QVERIFY(probe->vfr);
  }

  void constantFrameRateTimeMapping() {
    const CodecCase c = caseNamed(QStringLiteral("mpeg4_bframes"));
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    QVERIFY(!dec->info().vfr);
    for (qint64 n = 0; n < 100; ++n) {
      QVERIFY(qAbs(dec->timeOfFrame(n) - n / 25.0) < 1e-6);
      // time N/fps must land exactly on frame N despite floating point rounding
      QCOMPARE(dec->indexAtTime(n / 25.0), n);
      QCOMPARE(dec->indexAtTime(n / 25.0 + 0.019), n);
    }
    QVERIFY(qAbs(dec->info().avgFps - 25.0) < 0.01);
    QVERIFY(qAbs(dec->info().durationSec - 4.0) < 0.001);
  }

  void startOffsetIsRelativeToContainer() {
    const QString path = dir.filePath(QStringLiteral("offset.mkv"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-f", "lavfi", "-i", sf::test::indexSource(320, 240, 25, 3), "-f", "lavfi", "-i", "sine=frequency=440:duration=3",
                                  "-vf", "setpts=PTS+1/TB", "-c:v", "libopenh264", "-g", "20", "-c:a", "pcm_s16le", path},
                                 &log),
             qPrintable(log));
    auto dec = VideoDecoder::open(path, {HwMode::Off, 0});
    QVERIFY(dec);
    QVERIFY2(qAbs(dec->info().startSec - 1.0) < 0.002, qPrintable(QString::number(dec->info().startSec)));
    QVERIFY(qAbs(dec->timeOfFrame(10) - 1.4) < 0.002);
    QCOMPARE(dec->indexAtTime(0.5), 0); // before the first frame: nothing earlier to show
    QCOMPARE(dec->indexAtTime(1.0 + 10 / 25.0), 10);
    checkFrame(*dec, 10, "offset");
    const auto probe = probeMedia(path);
    QVERIFY(probe);
    QVERIFY(qAbs(probe->videoStartSec - 1.0) < 0.002);
    QVERIFY(qAbs(probe->audioStartSec) < 0.002);
  }

  void rotationIsReportedNotApplied() {
    const CodecCase c = caseNamed(QStringLiteral("mpeg4_bframes"));
    const QString clip = clipFor(c);
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    const QString rot = dir.filePath(QStringLiteral("rot90.mp4"));
    QString log;
    QVERIFY2(sf::test::runFfmpeg({"-display_rotation", "-90", "-i", clip, "-c", "copy", rot}, &log), qPrintable(log));
    auto dec = VideoDecoder::open(rot, {HwMode::Off, 0});
    QVERIFY(dec);
    QCOMPARE(dec->info().rotation, 90);
    const VideoFramePtr f = dec->frameAt(12);
    QVERIFY(f);
    QCOMPARE(f->image.size(), QSize(320, 240)); // stored orientation; the compositor rotates
    QCOMPARE(readIndex(f->image), 12);
  }

  // Limited-range video must expand to full range RGB (black 16 -> 0, white 235 -> 255).
  void levelsAreFullRange_data() {
    QTest::addColumn<QString>("codec");
    QTest::newRow("mpeg4") << "mpeg4_bframes";
    QTest::newRow("prores") << "prores";
    QTest::newRow("h264") << "h264_sw";
  }
  void levelsAreFullRange() {
    QFETCH(QString, codec);
    const QString clip = clipFor(caseNamed(codec));
    if (clip.isEmpty()) QSKIP(qPrintable(skipReason));
    auto dec = VideoDecoder::open(clip, {HwMode::Off, 0});
    QVERIFY(dec);
    const VideoFramePtr black = dec->frameAt(0); // N = 0: every block black
    QVERIFY(black);
    const QColor k = black->image.pixelColor(160, 120);
    QVERIFY2(k.red() <= 6 && k.green() <= 6 && k.blue() <= 6, qPrintable(k.name()));
    const VideoFramePtr white = dec->frameAt(31); // N = 31: blocks 0..4 white
    QVERIFY(white);
    const QColor w = white->image.pixelColor(10, 120);
    QVERIFY2(w.red() >= 250 && w.green() >= 250 && w.blue() >= 250, qPrintable(w.name()));
  }

  // Untagged HD video is BT.709, tagged video follows its tag.
  void colourMatrixDefaults() {
    for (const bool tagged : {false, true}) {
      const QString path = dir.filePath(QStringLiteral("red709_%1.mp4").arg(tagged));
      QString vf = QStringLiteral("scale=out_color_matrix=bt709:out_range=tv,format=yuv420p");
      if (tagged) vf += QStringLiteral(",setparams=colorspace=bt709:range=tv");
      const QStringList args{"-f", "lavfi", "-i", "color=c=0xC83232:s=1280x720:r=25:d=1", "-vf", vf, "-c:v", "mpeg4", "-q:v", "1", path};
      QString log;
      QVERIFY2(sf::test::runFfmpeg(args, &log), qPrintable(log));
      auto dec = VideoDecoder::open(path, {HwMode::Off, 0});
      QVERIFY(dec);
      const VideoFramePtr f = dec->frameAt(3);
      QVERIFY(f);
      const QColor c = f->image.pixelColor(640, 360);
      QVERIFY2(qAbs(c.red() - 200) <= 6 && qAbs(c.green() - 50) <= 6 && qAbs(c.blue() - 50) <= 6,
               qPrintable(QStringLiteral("tagged=%1 got %2").arg(tagged).arg(c.name())));
    }
  }

  void missingFileFailsCleanly() {
    QString err;
    QVERIFY(!VideoDecoder::open(dir.filePath(QStringLiteral("nope.mp4")), {}, &err));
    QVERIFY(!err.isEmpty());
  }
};

QTEST_GUILESS_MAIN(TstVideoDecoder)
#include "tst_video_decoder.moc"
