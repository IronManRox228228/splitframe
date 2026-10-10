// Export: tiny projects (<= 320x180, <= 2 s) go through the exporter and come back through the probe and the
// decoders. Needs D3D11 for the compositor (renders are 320x180); the suite skips without it. NVENC is opt-in
// (SF_TEST_NVENC=1) and never runs by default.

#include "audio/engine.h"
#include "audio/loudness.h"
#include "audio/source.h"
#include "audio/timebase.h"
#include "export/exporter.h"
#include "media/audio_decoder.h"
#include "media/probe.h"
#include "media/video_decoder.h"
#include "render/offscreen.h"
#include "render_testutil.h"

#include <QFile>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <cstring>
#include <set>

using namespace sf;
using namespace sf::render;
using namespace sf::test;
using namespace sf::xport;

namespace {

constexpr int kW = 320, kH = 180, kFps = 25, kFrames = 50;

QString ffprobeExe() { return QFileInfo(ffmpegExe()).absolutePath() + QStringLiteral("/ffprobe.exe"); }

QImage decodeAt(const QString& path, qint64 index) {
  VideoOpenOptions o;
  o.hw = HwMode::Off;
  QString err;
  auto d = VideoDecoder::open(path, o, &err);
  if (!d) return {};
  const auto f = d->frameAt(index);
  return f ? f->image : QImage();
}

qint64 audioSampleCount(const QString& path) {
  QString err;
  auto d = AudioDecoder::open(path, &err);
  if (!d) return -1;
  qint64 total = 0, pos = 0;
  std::vector<float> buf(48000 * 2);
  for (;;) {
    const qint64 got = d->read(pos, 48000, buf.data());
    if (got <= 0) break;
    total += got;
    pos += got;
  }
  return total;
}

// float32 stereo WAV -> samples
std::vector<float> readWavFloat(const QString& path, int* format = nullptr, int* bits = nullptr) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  const QByteArray all = f.readAll();
  qsizetype pos = 12;
  std::vector<float> out;
  while (pos + 8 <= all.size()) {
    const QByteArray id = all.mid(pos, 4);
    quint32 size;
    std::memcpy(&size, all.constData() + pos + 4, 4);
    if (id == "fmt ") {
      quint16 fmt, b;
      std::memcpy(&fmt, all.constData() + pos + 8, 2);
      std::memcpy(&b, all.constData() + pos + 8 + 14, 2);
      if (format) *format = fmt;
      if (bits) *bits = b;
    } else if (id == "data") {
      const qsizetype n = std::min<qsizetype>(size, all.size() - pos - 8);
      out.resize(static_cast<size_t>(n) / sizeof(float));
      std::memcpy(out.data(), all.constData() + pos + 8, out.size() * sizeof(float));
      return out;
    }
    pos += 8 + size + (size & 1);
  }
  return out;
}

QByteArray run(const QString& exe, const QStringList& args) {
  QProcess p;
  p.start(exe, args);
  p.waitForFinished(60000);
  return p.readAllStandardOutput();
}

// luma row of the first frame, decoded straight to the stored bit depth (no RGB round trip)
std::vector<int> lumaRow(const QString& path, int bits, int row) {
  const QByteArray raw = run(ffmpegExe(), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), path, QStringLiteral("-frames:v"), QStringLiteral("1"),
                                           QStringLiteral("-f"), QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"), bits > 8 ? QStringLiteral("gray10le") : QStringLiteral("gray"), QStringLiteral("-")});
  std::vector<int> v;
  const int bytes = bits > 8 ? 2 : 1;
  for (int x = 0; x < kW; ++x) {
    const qsizetype at = (static_cast<qsizetype>(row) * kW + x) * bytes;
    if (at + bytes > raw.size()) break;
    int s = static_cast<uchar>(raw[at]);
    if (bytes == 2) s |= static_cast<uchar>(raw[at + 1]) << 8;
    v.push_back(s);
  }
  return v;
}

} // namespace

class TstExport : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString src_, sync_, ramp_;
  bool gpuOk_ = false;
  QString skip_;

  struct Scene {
    TimelineDoc doc;
    AssetTable assets;
  };
  Scene indexScene(Frame frames = kFrames) {
    Scene s;
    s.doc = newDoc(kW, kH, kFps);
    s.assets["ast_a"] = {src_, AssetKind::Video};
    addMedia(s.doc, ItemType::Video, mainTrack(s.doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, frames);
    return s;
  }
  QString out(const QString& name) { return dir_.filePath(name); }

  ExportSettings settings(const QString& file) {
    ExportSettings s;
    s.outputPath = out(file);
    s.overwrite = true;
    s.encoderPreset = QStringLiteral("ultrafast");
    return s;
  }

  ExportResult exportScene(const Scene& s, const ExportSettings& st) { return runExport(s.doc, s.assets, st); }

  QImage referenceFrame(const Scene& s, Frame f) {
    QString err;
    auto off = OffscreenRenderer::create(&err);
    if (!off) return {};
    FrameService::Options o;
    o.hw = HwMode::Off;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    return off->render(s.doc, f, prov);
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    QString err;
    gpuOk_ = OffscreenRenderer::create(&err) != nullptr;
    if (!gpuOk_) skip_ = QStringLiteral("no D3D11 device: %1").arg(err);
    src_ = out(QStringLiteral("src.mp4"));
    QString log;
    QVERIFY2(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), indexSource(kW, kH, kFps, 2),
                        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"), QStringLiteral("sine=frequency=440:sample_rate=48000:duration=2"),
                        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                        QStringLiteral("-g"), QStringLiteral("25"), QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"), src_}, &log),
             qPrintable(log));
    // a white flash on frame 25 and a 1 kHz beep starting at 1.000 s, PCM so the source itself has no codec delay
    sync_ = out(QStringLiteral("sync.mov"));
    QVERIFY2(runFfmpeg({QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                        QStringLiteral("color=c=black:s=320x180:r=25:d=2,drawbox=x=0:y=0:w=320:h=180:color=white:t=fill:enable='eq(n,25)',format=yuv420p"),
                        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                        QStringLiteral("sine=frequency=1000:sample_rate=48000:duration=0.1,adelay=1000|1000,apad=whole_dur=2"),
                        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"), QStringLiteral("-qp"), QStringLiteral("0"),
                        QStringLiteral("-c:a"), QStringLiteral("pcm_s16le"), QStringLiteral("-shortest"), sync_}, &log),
             qPrintable(log));
    // a horizontal gray ramp, one 8-bit level per 1.25 px
    QImage ramp(kW, kH, QImage::Format_RGB32);
    for (int x = 0; x < kW; ++x) {
      const int v = x * 255 / (kW - 1);
      for (int y = 0; y < kH; ++y) ramp.setPixel(x, y, qRgb(v, v, v));
    }
    ramp_ = out(QStringLiteral("ramp.png"));
    QVERIFY(ramp.save(ramp_));
  }

  // ---------------------------------------------------------------- planning (no GPU)
  void presetsAndPlan() {
    int w = 0, h = 0;
    resolveTierSize(720, 1920, 1080, &w, &h);
    QCOMPARE(w, 1280);
    QCOMPARE(h, 720);
    resolveTierSize(720, 1080, 1920, &w, &h); // short side named, vertical canvas stays vertical
    QCOMPARE(w, 720);
    QCOMPARE(h, 1280);
    ExportSettings s;
    QVERIFY(applyPreset(s, QStringLiteral("720p"), 1920, 1080));
    QCOMPARE(s.width, 1280);
    QCOMPARE(s.videoBitrateK, 5300); // Electron: 12000 * 921600 / 2073600 -> 5300
    QVERIFY(applyPreset(s, QStringLiteral("vertical"), 1920, 1080));
    QCOMPARE(s.width, 1080);
    QCOMPARE(s.height, 1920);
    QVERIFY(!applyPreset(s, QStringLiteral("nonsense"), 1920, 1080));
    QVERIFY(applyQuality(s, QStringLiteral("high")));
    QCOMPARE(s.crf, 18);

    TimelineDoc doc = newDoc(kW, kH, kFps);
    addMedia(doc, ItemType::Video, mainTrack(doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, 50);
    QString err;
    ExportSettings a;
    a.outputPath = QStringLiteral("x.mp4");
    auto p = planExport(doc, a, &err);
    QVERIFY2(p.has_value(), qPrintable(err));
    QCOMPARE(p->frames, 50);
    QCOMPARE(p->audioSamples, 96000);
    QCOMPARE(p->width, kW);
    QCOMPARE(p->bits, 8);
    QCOMPARE(p->transfer, QStringLiteral("bt709"));
    // vertical canvas from a horizontal project: re-laid out, not stretched
    a.width = 180;
    a.height = 320;
    p = planExport(doc, a, &err);
    QVERIFY(p.has_value());
    QCOMPARE(p->canvasWidth, 180);
    // same aspect: scaled after rendering
    a.width = 160;
    a.height = 90;
    p = planExport(doc, a, &err);
    QVERIFY(p.has_value());
    QCOMPARE(p->canvasWidth, kW);
    QCOMPARE(p->width, 160);
    // refusals
    a = ExportSettings();
    a.outputPath = QStringLiteral("x.mp4");
    a.inFrame = 10;
    a.outFrame = 10;
    QVERIFY(!planExport(doc, a, &err) && err.contains(QStringLiteral("empty")));
    a = ExportSettings();
    a.outputPath = QStringLiteral("x.xyz");
    QVERIFY(!planExport(doc, a, &err) && err.contains(QStringLiteral("container")));
    a = ExportSettings();
    a.outputPath = QStringLiteral("x.mp4");
    a.width = 161;
    a.height = 91;
    QVERIFY(!planExport(doc, a, &err) && err.contains(QStringLiteral("even")));
    a = ExportSettings();
    a.outputPath = QStringLiteral("x.mp4");
    a.video = VideoCodec::ProRes;
    QVERIFY(!planExport(doc, a, &err));
    doc.project.colorManagement = ColorManagement{};
    doc.project.colorManagement->outputSpace = QStringLiteral("rec2100pq");
    a = ExportSettings();
    a.outputPath = QStringLiteral("x.mp4");
    QVERIFY(!planExport(doc, a, &err) && err.contains(QStringLiteral("HDR")));
    a.video = VideoCodec::H265;
    p = planExport(doc, a, &err);
    QVERIFY(p.has_value());
    QCOMPARE(p->bits, 10);
    QCOMPARE(p->transfer, QStringLiteral("smpte2084"));
    QCOMPARE(p->primaries, QStringLiteral("bt2020"));
  }

  // ---------------------------------------------------------------- codecs
  void x264Mp4() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("x264.mp4"));
    st.crf = 14;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.videoFrames, kFrames);
    QCOMPARE(r.videoEncoder, QStringLiteral("libx264"));
    QVERIFY(!QFileInfo::exists(st.outputPath + QStringLiteral(".sfpart")));

    QString err;
    const auto info = probeMedia(st.outputPath, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY2(info.has_value(), qPrintable(err));
    QCOMPARE(info->videoCodec, QStringLiteral("h264"));
    QCOMPARE(info->width, kW);
    QCOMPARE(info->height, kH);
    QCOMPARE(info->frameCount, kFrames);
    QVERIFY(!info->vfr);
    QVERIFY(std::abs(info->avgFps - kFps) < 0.01);
    QVERIFY2(std::abs(info->durationMs - 2000) <= 30, qPrintable(QString::number(info->durationMs)));
    QVERIFY(info->hasAudio);
    QCOMPARE(info->audioSampleRate, 48000);
    QCOMPARE(info->audioCodec, QStringLiteral("aac"));
    QCOMPARE(info->color.matrix, QStringLiteral("bt709"));
    QCOMPARE(info->color.range, QStringLiteral("tv"));
    const qint64 samples = audioSampleCount(st.outputPath);
    QVERIFY2(std::abs(samples - 96000) <= 2048, qPrintable(QString::number(samples)));

    // faststart: the index precedes the media data
    QFile f(st.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray head = f.read(64 * 1024);
    const qsizetype moov = head.indexOf("moov"), mdat = head.indexOf("mdat");
    QVERIFY2(moov > 0 && (mdat < 0 || moov < mdat), "moov should come before mdat");

    // first and last frame against the compositor's own render
    for (const Frame fr : {Frame(0), Frame(kFrames - 1)}) {
      const QImage got = decodeAt(st.outputPath, fr), want = referenceFrame(s, fr);
      QVERIFY(!got.isNull() && !want.isNull());
      QCOMPARE(readIndex(got), static_cast<int>(fr));
      const double d = meanAbsDiff(got, want);
      QVERIFY2(d < 4.0, qPrintable(QStringLiteral("frame %1 differs by %2").arg(fr).arg(d)));
    }
  }

  void x265Mkv() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("x265.mkv"));
    st.video = VideoCodec::H265;
    st.crf = 16;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QString err;
    const auto info = probeMedia(st.outputPath, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY2(info.has_value(), qPrintable(err));
    QCOMPARE(info->videoCodec, QStringLiteral("hevc"));
    QCOMPARE(info->frameCount, kFrames);
    QCOMPARE(info->width, kW);
    QVERIFY(std::abs(info->avgFps - kFps) < 0.5);
    QVERIFY(info->hasAudio);
    QCOMPARE(info->pixelFormat, QStringLiteral("yuv420p"));
    QCOMPARE(readIndex(decodeAt(st.outputPath, 7)), 7);
    QCOMPARE(readIndex(decodeAt(st.outputPath, kFrames - 1)), kFrames - 1);
  }

  void proresMov() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("prores.mov"));
    st.video = VideoCodec::ProRes;
    st.audio = AudioCodec::Pcm16;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QString err;
    const auto info = probeMedia(st.outputPath, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY2(info.has_value(), qPrintable(err));
    QCOMPARE(info->videoCodec, QStringLiteral("prores"));
    QCOMPARE(info->frameCount, kFrames);
    QCOMPARE(info->pixelFormat, QStringLiteral("yuv422p10le"));
    QCOMPARE(info->bitDepth, 10);
    QCOMPARE(info->audioCodec, QStringLiteral("pcm_s16le"));
    QCOMPARE(audioSampleCount(st.outputPath), 96000);
    QCOMPARE(readIndex(decodeAt(st.outputPath, 33)), 33);
  }

  void dnxhrMov() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene(10);
    ExportSettings st = settings(QStringLiteral("dnx.mov"));
    st.video = VideoCodec::DnxHr;
    st.audio = AudioCodec::Pcm24;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QString err;
    const auto info = probeMedia(st.outputPath, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY2(info.has_value(), qPrintable(err));
    QCOMPARE(info->videoCodec, QStringLiteral("dnxhd"));
    QCOMPARE(info->frameCount, 10);
    QCOMPARE(info->audioCodec, QStringLiteral("pcm_s24le"));
    QCOMPARE(readIndex(decodeAt(st.outputPath, 4)), 4);
  }

  // ---------------------------------------------------------------- time
  void avSync() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(kW, kH, kFps);
    s.assets["ast_a"] = {sync_, AssetKind::Video};
    addMedia(s.doc, ItemType::Video, mainTrack(s.doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, kFrames);
    for (const auto& [file, audio] : {std::pair{QStringLiteral("sync.mp4"), AudioCodec::Aac}, std::pair{QStringLiteral("sync.mov"), AudioCodec::Pcm16},
                                      std::pair{QStringLiteral("sync.mkv"), AudioCodec::Aac}}) {
      ExportSettings st = settings(QStringLiteral("out_") + file);
      st.audio = audio;
      st.crf = 12;
      const ExportResult r = exportScene(s, st);
      QVERIFY2(r.ok, qPrintable(r.error));
      VideoOpenOptions vo;
      vo.hw = HwMode::Off;
      QString err;
      auto vd = VideoDecoder::open(st.outputPath, vo, &err);
      auto ad = AudioDecoder::open(st.outputPath, &err);
      QVERIFY2(vd && ad, qPrintable(err));
      int flash = -1;
      for (int i = 0; i < kFrames; ++i) {
        const auto f = vd->frameAt(i);
        QVERIFY(f);
        if (qGray(f->image.pixel(kW / 2, kH / 2)) > 200) {
          flash = i;
          break;
        }
      }
      QCOMPARE(flash, 25);
      const std::vector<float> a = ad->read(0, 96000);
      qint64 onset = -1;
      for (size_t i = 0; i < a.size(); ++i) {
        if (std::fabs(a[i]) > 0.02f) {
          onset = static_cast<qint64>(i / 2);
          break;
        }
      }
      QVERIFY(onset >= 0);
      const double tVideo = vd->info().startSec + flash / double(kFps);
      const double tAudio = ad->info().startSec + static_cast<double>(onset) / 48000.0;
      QVERIFY2(std::abs(tVideo - 1.0) < 0.02, qPrintable(QStringLiteral("%1: video flash at %2 s").arg(file).arg(tVideo)));
      QVERIFY2(std::abs(tAudio - tVideo) < 0.025, qPrintable(QStringLiteral("%1: beep at %2 s, flash at %3 s").arg(file).arg(tAudio).arg(tVideo)));
    }
  }

  void rangeExport() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("range.mov"));
    st.audio = AudioCodec::Pcm16;
    st.crf = 12;
    st.inFrame = 10;
    st.outFrame = 35;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.videoFrames, 25);
    QString err;
    const auto info = probeMedia(st.outputPath, &err, ProbeOptions{.vfrScanLimit = 0});
    QVERIFY(info.has_value());
    QCOMPARE(info->frameCount, 25);
    QVERIFY(std::abs(info->durationMs - 1000) <= 20);
    QCOMPARE(audioSampleCount(st.outputPath), 48000);
    QCOMPARE(readIndex(decodeAt(st.outputPath, 0)), 10);
    QCOMPARE(readIndex(decodeAt(st.outputPath, 24)), 34);
  }

  void resolutionPresets() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene(10);
    // same aspect: scaled; different aspect: re-laid out (contain-fitted)
    ExportSettings st = settings(QStringLiteral("small.mp4"));
    st.width = 160;
    st.height = 90;
    ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    auto info = probeMedia(st.outputPath);
    QVERIFY(info);
    QCOMPARE(info->width, 160);
    QCOMPARE(info->height, 90);
    st = settings(QStringLiteral("vertical.mp4"));
    st.width = 180;
    st.height = 320;
    r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    info = probeMedia(st.outputPath);
    QVERIFY(info);
    QCOMPARE(info->width, 180);
    QCOMPARE(info->height, 320);
  }

  // ---------------------------------------------------------------- lifecycle
  void cancelRemovesFile() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("cancel.mp4"));
    std::atomic<bool> cancel{false};
    const ExportResult r = runExport(s.doc, s.assets, st, [&](const ExportProgress&) { cancel = true; }, &cancel);
    QVERIFY(r.cancelled);
    QVERIFY(!r.ok);
    QVERIFY(!QFileInfo::exists(st.outputPath));
    QVERIFY(!QFileInfo::exists(st.outputPath + QStringLiteral(".sfpart")));
    // an existing file survives a cancelled overwrite
    ExportSettings keep = settings(QStringLiteral("keep.mp4"));
    QVERIFY(exportScene(s, keep).ok);
    const qint64 size = QFileInfo(keep.outputPath).size();
    cancel = false;
    const ExportResult r2 = runExport(s.doc, s.assets, keep, [&](const ExportProgress&) { cancel = true; }, &cancel);
    QVERIFY(r2.cancelled);
    QCOMPARE(QFileInfo(keep.outputPath).size(), size);
  }

  void overwriteRefused() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene(10);
    ExportSettings st = settings(QStringLiteral("over.mp4"));
    st.overwrite = false;
    QVERIFY(exportScene(s, st).ok);
    const QByteArray before = [&] {
      QFile f(st.outputPath);
      f.open(QIODevice::ReadOnly);
      return f.readAll();
    }();
    const ExportResult r = exportScene(s, st);
    QVERIFY(!r.ok);
    QVERIFY(!r.cancelled);
    QVERIFY2(r.error.contains(QStringLiteral("already exists")), qPrintable(r.error));
    {
      QFile f(st.outputPath);
      QVERIFY(f.open(QIODevice::ReadOnly));
      QCOMPARE(f.readAll(), before);
    } // closed: Windows will not replace an open file
    st.overwrite = true;
    QVERIFY(exportScene(s, st).ok);
  }

  void missingMediaIsReported() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    Scene s = indexScene(5);
    s.assets["ast_a"].path = out(QStringLiteral("nope.mp4"));
    ExportSettings st = settings(QStringLiteral("missing.mp4"));
    const ExportResult r = exportScene(s, st);
    QVERIFY(r.ok); // the "media unavailable" card is drawn, as in preview
    QCOMPARE(r.missingFrames, 5);
  }

  // ---------------------------------------------------------------- audio
  void audioOnlyWavIsBitExact() {
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("audio.wav"));
    st.audio = AudioCodec::Pcm32f;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.videoFrames, 0);
    QCOMPARE(r.audioSamples, 96000);
    int fmt = 0, bits = 0;
    const std::vector<float> got = readWavFloat(st.outputPath, &fmt, &bits);
    QCOMPARE(fmt, 3); // IEEE float
    QCOMPARE(bits, 32);
    auto files = std::make_shared<audio::FileProvider>();
    files->setFiles({{QStringLiteral("ast_a"), {src_, true}}});
    const std::vector<float> want = audio::renderRange(std::make_shared<const TimelineDoc>(s.doc), files, 0, 96000);
    QCOMPARE(got.size(), want.size());
    QVERIFY2(std::memcmp(got.data(), want.data(), got.size() * sizeof(float)) == 0, "the exported samples differ from renderRange");
    // a range starting mid-timeline matches too (PCM source: an AAC decoder seeking mid-stream differs from a linear
    // decode by its priming error, which is the decoder's, not the exporter's)
    Scene pcm;
    pcm.doc = newDoc(kW, kH, kFps);
    pcm.assets["ast_a"] = {sync_, AssetKind::Video};
    addMedia(pcm.doc, ItemType::Video, mainTrack(pcm.doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, kFrames);
    files->setFiles({{QStringLiteral("ast_a"), {sync_, true}}});
    st.inFrame = 7;
    st.outFrame = 31;
    QVERIFY(exportScene(pcm, st).ok);
    const std::vector<float> part = readWavFloat(st.outputPath);
    const std::int64_t a = audio::frameToSample(7, kFps), n = audio::frameToSample(31, kFps) - a;
    const std::vector<float> wantPart = audio::renderRange(std::make_shared<const TimelineDoc>(pcm.doc), files, a, n);
    QCOMPARE(part.size(), wantPart.size());
    size_t firstBad = part.size();
    float worst = 0;
    for (size_t i = 0; i < part.size(); ++i) {
      const float d = std::fabs(part[i] - wantPart[i]);
      if (d > 0 && firstBad == part.size()) firstBad = i;
      worst = std::max(worst, d);
    }
    QVERIFY2(firstBad == part.size(), qPrintable(QStringLiteral("first mismatch at sample %1 of %2, worst %3").arg(firstBad / 2).arg(part.size() / 2).arg(worst)));
    // 16-bit is the same signal to within one LSB
    st = settings(QStringLiteral("audio16.wav"));
    QVERIFY(exportScene(s, st).ok);
    QFile f(st.outputPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray all = f.readAll();
    const qsizetype dataAt = all.indexOf("data") + 8;
    const auto* p = reinterpret_cast<const qint16*>(all.constData() + dataAt);
    for (size_t i = 0; i < want.size(); i += 97) QVERIFY(std::abs(p[i] - want[i] * 32768.0f) <= 1.0f);
  }

  void loudnessNormalise() {
    const Scene s = indexScene();
    ExportSettings st = settings(QStringLiteral("loud.wav"));
    st.audio = AudioCodec::Pcm32f;
    st.loudnessLufs = -23.0;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    const std::vector<float> got = readWavFloat(st.outputPath);
    QVERIFY(!got.empty());
    const auto m = audio::measureLoudness(got.data(), static_cast<std::int64_t>(got.size() / 2));
    QVERIFY2(std::abs(m.integrated - (-23.0)) <= 0.5, qPrintable(QStringLiteral("integrated %1 LUFS (gain %2 dB)").arg(m.integrated).arg(r.appliedGainDb)));
    QVERIFY(std::abs(r.appliedGainDb) > 1.0); // the source is nowhere near -23
    // and with a true-peak ceiling the gain is held back
    st.loudnessLufs = -6.0;
    st.truePeakCeilingDb = -3.0;
    st.outputPath = out(QStringLiteral("loud2.wav"));
    QVERIFY(exportScene(s, st).ok);
    const std::vector<float> g2 = readWavFloat(st.outputPath);
    const auto m2 = audio::measureLoudness(g2.data(), static_cast<std::int64_t>(g2.size() / 2));
    QVERIFY2(m2.truePeakDb <= -2.9, qPrintable(QString::number(m2.truePeakDb)));
  }

  // ---------------------------------------------------------------- 10-bit and HDR
  void tenBitKeepsLevels() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(kW, kH, kFps);
    s.assets["ast_i"] = {ramp_, AssetKind::Image};
    addMedia(s.doc, ItemType::Image, mainTrack(s.doc), QStringLiteral("itm_i"), QStringLiteral("ast_i"), 0, 5);
    ExportSettings st = settings(QStringLiteral("ramp10.mkv"));
    st.video = VideoCodec::H265;
    st.tenBit = true;
    st.audio = AudioCodec::None;
    st.crf = 8;
    st.encoderPreset = QStringLiteral("medium");
    ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    auto info = probeMedia(st.outputPath);
    QVERIFY(info);
    QCOMPARE(info->pixelFormat, QStringLiteral("yuv420p10le"));
    QCOMPARE(info->bitDepth, 10);
    const std::vector<int> row10 = lumaRow(st.outputPath, 10, kH / 2);
    QCOMPARE(static_cast<int>(row10.size()), kW);
    std::set<int> distinct10(row10.begin(), row10.end());

    ExportSettings st8 = settings(QStringLiteral("ramp8.mkv"));
    st8.video = VideoCodec::H265;
    st8.audio = AudioCodec::None;
    st8.crf = 8;
    st8.encoderPreset = QStringLiteral("medium");
    r = exportScene(s, st8);
    QVERIFY2(r.ok, qPrintable(r.error));
    const std::vector<int> row8 = lumaRow(st8.outputPath, 8, kH / 2);
    std::set<int> distinct8(row8.begin(), row8.end());
    // limited-range 8-bit has at most 220 luma codes; the 10-bit ramp of 256 source levels must use more than that
    QVERIFY2(distinct8.size() <= 220, qPrintable(QString::number(distinct8.size())));
    QVERIFY2(distinct10.size() > 225, qPrintable(QStringLiteral("10-bit ramp has %1 levels").arg(distinct10.size())));
    // and the codes are 10-bit codes: black ~64, white ~940 (limited range)
    QVERIFY2(*distinct10.begin() < 80 && *distinct10.rbegin() > 920, qPrintable(QStringLiteral("%1..%2").arg(*distinct10.begin()).arg(*distinct10.rbegin())));
  }

  void hdrTagsAreWritten() {
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    Scene s = indexScene(10);
    s.doc.project.colorManagement = ColorManagement{};
    s.doc.project.colorManagement->outputSpace = QStringLiteral("rec2100pq");
    ExportSettings st = settings(QStringLiteral("pq.mp4"));
    st.video = VideoCodec::H265;
    st.crf = 18;
    ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    auto info = probeMedia(st.outputPath);
    QVERIFY(info);
    QCOMPARE(info->color.transfer, QStringLiteral("smpte2084"));
    QCOMPARE(info->color.primaries, QStringLiteral("bt2020"));
    QCOMPARE(info->color.matrix, QStringLiteral("bt2020nc"));
    QCOMPARE(info->color.range, QStringLiteral("tv"));
    QVERIFY(info->color.isHdr());
    QCOMPARE(info->pixelFormat, QStringLiteral("yuv420p10le"));
    // mastering display + content light level reached the container
    const QByteArray probe = run(ffprobeExe(), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_streams"), QStringLiteral("-select_streams"), QStringLiteral("v:0"), st.outputPath});
    QVERIFY2(probe.contains("Mastering display metadata"), probe.constData());
    QVERIFY2(probe.contains("Content light level metadata"), probe.constData());

    s.doc.project.colorManagement->outputSpace = QStringLiteral("rec2100hlg");
    st = settings(QStringLiteral("hlg.mov"));
    st.video = VideoCodec::ProRes;
    r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    info = probeMedia(st.outputPath);
    QVERIFY(info);
    QCOMPARE(info->color.transfer, QStringLiteral("arib-std-b67"));
    QCOMPARE(info->color.primaries, QStringLiteral("bt2020"));
    // an HDR project cannot go to 8-bit H.264: refused, no file
    st = settings(QStringLiteral("hdr264.mp4"));
    r = exportScene(s, st);
    QVERIFY(!r.ok);
    QVERIFY(!QFileInfo::exists(st.outputPath));
  }

  // ---------------------------------------------------------------- hardware (opt-in)
  void nvencOptIn() {
    if (qEnvironmentVariable("SF_TEST_NVENC") != QLatin1String("1")) QSKIP("NVENC tests only run with SF_TEST_NVENC=1");
    if (!gpuOk_) QSKIP(qPrintable(skip_));
    const Scene s = indexScene(10);
    ExportSettings st = settings(QStringLiteral("nvenc.mp4"));
    st.hardware = Hardware::Auto;
    const ExportResult r = exportScene(s, st);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.videoEncoder == QLatin1String("h264_nvenc") || r.videoEncoder == QLatin1String("libx264"));
  }
};

QTEST_MAIN(TstExport)
#include "tst_export.moc"
