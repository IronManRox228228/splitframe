// Colour pipeline: transfer functions (CPU reference vs the shaders), linear vs display blending,
// delivery encodings, 10-bit smoothness and the OpenColorIO paths (config, LUT files, log inputs,
// working space, display transforms). Rendering is offscreen on D3D11; clips come from the ffmpeg CLI.

#include "core/apply.h"
#include "render/color.h"
#include "render/color_manager.h"
#include "render/offscreen.h"
#include "render_testutil.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <cmath>
#include <set>

using namespace sf;
using namespace sf::render;
using namespace sf::test;

namespace {

struct Rgb8 {
  int r, g, b;
};

std::array<float, 3> floatPixel(const QImage& img, int x, int y) {
  const auto* p = reinterpret_cast<const float*>(img.constScanLine(y)) + x * 4;
  return {p[0], p[1], p[2]};
}

// 16-bit RGBA64 pixel scaled to `maxCode` (255 / 1023)
std::array<int, 3> codes(const QImage& img, int x, int y, int maxCode) {
  const auto* p = reinterpret_cast<const quint16*>(img.constScanLine(y)) + x * 4;
  std::array<int, 3> c{};
  for (size_t k = 0; k < 3; ++k) c[k] = static_cast<int>(std::lround(p[k] * double(maxCode) / 65535.0));
  return c;
}

} // namespace

class TstColor : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  std::unique_ptr<OffscreenRenderer> gpu_;
  std::shared_ptr<ColorManager> cm_; // the built-in studio config
  QString skip_;
  QString cmError_;
  QHash<QString, QString> clips_;

  struct Scene {
    TimelineDoc doc;
    AssetTable assets;
  };

  Compositor& comp() { return gpu_->compositor(); }

  QString savePng(const QString& name, const QImage& img) {
    const QString path = dir_.filePath(name + QStringLiteral(".png"));
    if (!img.save(path)) return {};
    return path;
  }

  // Images of any size as one item filling a same-size canvas
  Scene imageScene(const QString& png, int w, int h, const QString& bg = QStringLiteral("#000000")) {
    Scene s;
    s.doc = newDoc(w, h, 25, bg);
    s.assets["ast_img"] = {png, AssetKind::Image};
    addMedia(s.doc, ItemType::Image, mainTrack(s.doc), QStringLiteral("itm_i"), QStringLiteral("ast_img"), 0, 30);
    return s;
  }

  static void setColor(Scene& s, const std::function<void(ColorManagement&)>& f) {
    ColorManagement cm = s.doc.project.colorManagement.value_or(ColorManagement{});
    f(cm);
    s.doc.project.colorManagement = cm;
  }

  enum class Out { Rgba8, Rgba64, Linear };
  QImage run(const Scene& s, Out what, Frame frame = 0, RenderStats* stats = nullptr) {
    FrameService::Options o;
    o.hw = HwMode::Off;
    o.gpuFrames = false;
    o.cacheBytes = 64ll << 20;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    comp().setGpuEnabled(false);
    switch (what) {
    case Out::Rgba8: return gpu_->render(s.doc, frame, prov, stats);
    case Out::Rgba64: return gpu_->renderRgba64(s.doc, frame, prov, stats);
    case Out::Linear: return gpu_->renderLinear(s.doc, frame, prov, stats);
    }
    return {};
  }

  // The preview: render() then present() into an RGBA8 texture of the canvas' size
  QImage preview(const Scene& s) {
    FrameService::Options o;
    o.hw = HwMode::Off;
    o.gpuFrames = false;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    comp().setGpuEnabled(false);
    comp().setDeliveryEnabled(false);
    QRhi* rhi = gpu_->rhi();
    const QSize size(static_cast<int>(s.doc.project.width), static_cast<int>(s.doc.project.height));
    std::unique_ptr<QRhiTexture> tex(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!tex->create()) return {};
    std::unique_ptr<QRhiTextureRenderTarget> rt(rhi->newTextureRenderTarget({tex.get()}));
    std::unique_ptr<QRhiRenderPassDescriptor> rpd(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rpd.get());
    if (!rt->create()) return {};
    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return {};
    comp().render(cb, s.doc, 0, prov);
    comp().present(cb, rt.get(), Qt::black);
    QRhiReadbackResult result;
    QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
    u->readBackTexture(QRhiReadbackDescription(tex.get()), &result);
    cb->resourceUpdate(u);
    rhi->endOffscreenFrame();
    comp().setDeliveryEnabled(true);
    if (result.data.isEmpty()) return {};
    return QImage(reinterpret_cast<const uchar*>(result.data.constData()), size.width(), size.height(), QImage::Format_RGBA8888).copy();
  }

  // A horizontal grey ramp clip: R = G = B = x, tagged as given. Lossless planar RGB (no matrix or range to
  // get in the way), software decoded.
  QString rampClip(const QString& name, const QString& trc, const QString& primaries) {
    const QString key = name;
    if (const auto it = clips_.find(key); it != clips_.end()) return it.value();
    const QString path = dir_.filePath(name + QStringLiteral(".mkv"));
    QString log;
    const QStringList codec{"-c:v", "ffv1", "-pix_fmt", "gbrp"};
    // setparams puts the tags on the frames; the muxer writes them (the -color_trc output option is not kept for ffv1)
    const QString graph = QStringLiteral("color=c=gray:s=256x16:r=25:d=1,format=gbrp,geq=r='X':g='X':b='X',setparams=color_trc=%1:color_primaries=%2").arg(trc, primaries);
    const bool ok = makeClip(path, graph, codec, {}, &log);
    if (!ok) skip_ = QStringLiteral("cannot encode %1: %2").arg(name, log.left(200));
    clips_.insert(key, ok ? path : QString());
    return clips_.value(key);
  }

  Scene videoScene(const QString& path, int w, int h) {
    Scene s;
    s.doc = newDoc(w, h);
    s.assets["ast_v"] = {path, AssetKind::Video};
    addMedia(s.doc, ItemType::Video, mainTrack(s.doc), QStringLiteral("itm_v"), QStringLiteral("ast_v"), 0, 20);
    return s;
  }

  static QImage greyRamp() {
    QImage img(256, 8, QImage::Format_RGBA8888);
    for (int y = 0; y < img.height(); ++y) {
      for (int x = 0; x < 256; ++x) img.setPixelColor(x, y, QColor(x, x, x));
    }
    return img;
  }

  // 16 test colours in a 16x1 strip, upscaled to 16x8 blocks of 1 pixel wide
  static QImage palette(const std::vector<Rgb8>& colours) {
    QImage img(static_cast<int>(colours.size()), 4, QImage::Format_RGBA8888);
    for (int x = 0; x < img.width(); ++x) {
      for (int y = 0; y < img.height(); ++y) {
        const Rgb8& c = colours[static_cast<size_t>(x)];
        img.setPixelColor(x, y, QColor(c.r, c.g, c.b));
      }
    }
    return img;
  }

  static std::vector<Rgb8> sampleColours() {
    return {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {128, 128, 128}, {200, 120, 40}, {30, 60, 200}, {10, 10, 10},
            {250, 250, 250}, {90, 160, 120}, {255, 128, 0}, {60, 0, 120}, {180, 180, 60}, {20, 200, 220}, {100, 100, 100}, {64, 32, 16}};
  }

  static double expectedLin(Transfer t, int v8) { return toLinear(t, v8 / 255.0); }
  static int to8(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); }

  // Writes a .cube file; `f` maps an input triplet in 0..1 to the output triplet
  QString writeCube(const QString& name, int size, const std::function<std::array<double, 3>(double, double, double)>& f) {
    const QString path = dir_.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&file);
    out << "TITLE \"test\"\nLUT_3D_SIZE " << size << "\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    for (int b = 0; b < size; ++b) {
      for (int g = 0; g < size; ++g) {
        for (int r = 0; r < size; ++r) {
          const auto v = f(double(r) / (size - 1), double(g) / (size - 1), double(b) / (size - 1));
          out << QString::number(v[0], 'f', 6) << ' ' << QString::number(v[1], 'f', 6) << ' ' << QString::number(v[2], 'f', 6) << '\n';
        }
      }
    }
    return path;
  }

  QString findSpace(const QRegularExpression& re) const {
    for (const QString& s : cm_->colorSpaces()) {
      if (re.match(s).hasMatch()) return s;
    }
    return {};
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    QString err;
    gpu_ = OffscreenRenderer::create(&err);
    if (!gpu_) skip_ = QStringLiteral("no D3D11 QRhi: %1").arg(err);
    if (ColorManager::available()) {
      std::unique_ptr<ColorManager> m = ColorManager::create({}, &cmError_);
      cm_ = std::move(m);
    }
  }

  void init() {
    if (!gpu_) return;
    comp().setColorManager(nullptr);
    comp().setBlendSpace(std::nullopt);
    comp().setDelivery(std::nullopt);
    comp().setSdrTransfer(Transfer::Srgb);
    comp().setDeliveryEnabled(true);
  }

  // ---- CPU references (no GPU) ----

  void transferFunctionsRoundTrip() {
    for (const Transfer t : {Transfer::Srgb, Transfer::Bt709, Transfer::Bt1886, Transfer::Gamma22, Transfer::Gamma28, Transfer::Pq, Transfer::Hlg, Transfer::Linear}) {
      for (int i = 0; i <= 255; ++i) {
        const double v = i / 255.0;
        const double back = fromLinear(t, toLinear(t, v));
        // PQ has a tiny black floor (0 encodes to 7e-7)
        QVERIFY2(std::abs(back - v) < 1e-9 + 1e-6 * v + (t == Transfer::Pq ? 1e-6 : 0), qPrintable(QStringLiteral("transfer %1 at %2: %3").arg(int(t)).arg(v).arg(back)));
      }
    }
    // anchors from the standards
    QVERIFY(std::abs(toLinear(Transfer::Srgb, 0.5) - 0.21404) < 1e-4);
    QVERIFY(std::abs(toLinear(Transfer::Srgb, 0.04) - 0.04 / 12.92) < 1e-12);
    QVERIFY(std::abs(toLinear(Transfer::Bt709, 0.5) - 0.2594) < 1e-3);
    QVERIFY(std::abs(toLinear(Transfer::Bt1886, 0.5) - 0.18946) < 1e-4);
    QVERIFY(std::abs(fromLinear(Transfer::Pq, 1.0) - 0.5807) < 1e-3); // 203 nit = 58% PQ (BT.2408)
    QVERIFY(std::abs(toLinear(Transfer::Pq, 1.0) - 10000.0 / 203.0) < 1e-6);
    QVERIFY(std::abs(toLinear(Transfer::Hlg, 0.75) - 1.0) < 0.01); // HLG 75% signal is reference white (203 nit at a 1000 nit display)
    QVERIFY(std::abs(toLinear(Transfer::Hlg, 1.0) - 1000.0 / 203.0) < 1e-3);
    // HLG's OOTF works on luminance: a colour round-trips as a whole
    const Rgbd c = {0.8, 0.3, 0.1};
    const Rgbd back = decodeRgb(Transfer::Hlg, encodeRgb(Transfer::Hlg, c));
    for (size_t k = 0; k < 3; ++k) QVERIFY(std::abs(back[k] - c[k]) < 1e-9);
  }

  void primariesAndToneMap() {
    const Mat3 m = primariesMatrix(Primaries::Rec2020, Primaries::Rec709);
    // the published BT.2020 -> BT.709 matrix (BT.2087)
    const double want[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};
    for (size_t i = 0; i < 9; ++i) QVERIFY2(std::abs(m[i] - want[i]) < 2e-3, qPrintable(QString::number(i)));
    const Rgbd white = mulMat3(primariesMatrix(Primaries::Rec709, Primaries::DisplayP3), {1, 1, 1});
    for (double v : white) QVERIFY(std::abs(v - 1) < 1e-6); // D65 white maps to white
    const Mat3 id = multiply(primariesMatrix(Primaries::Rec709, Primaries::Rec2020), primariesMatrix(Primaries::Rec2020, Primaries::Rec709));
    for (size_t i = 0; i < 9; ++i) QVERIFY(std::abs(id[i] - kIdentity3[i]) < 1e-9);

    QCOMPARE(toneMapHighlights({0.5, 0.2, 0.1}), (Rgbd{0.5, 0.2, 0.1})); // below the knee: untouched
    const Rgbd bright = toneMapHighlights({4.0, 2.0, 1.0});
    QVERIFY(bright[0] < 1.0 && bright[0] > 0.99);
    QVERIFY(std::abs(bright[1] / bright[0] - 0.5) < 1e-9); // hue preserved
    double prev = 0;
    for (double v = 0.7; v < 20; v += 0.05) { // monotonic
      const double o = toneMapHighlights({v, v, v})[0];
      QVERIFY(o + 1e-12 >= prev);
      prev = o;
    }
    QCOMPARE(deliveryFromSpace(QStringLiteral("rec2100PQ")).encoding, Delivery::Encoding::Rec2100Pq);
    QCOMPARE(deliveryFromSpace(QStringLiteral("Rec.1886 Rec.709 - Display")).ocioSpace, QStringLiteral("Rec.1886 Rec.709 - Display"));
  }

  // ---- the shaders against the CPU reference ----

  // sRGB sources go through the float canvas and out again without changing: an opaque layer is bit exact
  void srgbSourceIsBitExact() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    const QString png = savePng(QStringLiteral("ramp"), greyRamp());
    QVERIFY(!png.isEmpty());
    for (const BlendSpace space : {BlendSpace::Linear, BlendSpace::Display}) {
      Scene s = imageScene(png, 256, 8);
      setColor(s, [&](ColorManagement& c) { c.blendSpace = space; });
      const QImage img = run(s, Out::Rgba8);
      int bad = 0;
      for (int x = 0; x < 256; ++x) {
        if (qRed(img.pixel(x, 4)) != x || qGreen(img.pixel(x, 4)) != x || qBlue(img.pixel(x, 4)) != x) ++bad;
      }
      QVERIFY2(bad == 0, qPrintable(QStringLiteral("%1 of 256 grey levels changed (blend space %2)").arg(bad).arg(int(space))));
    }
    // and the float canvas holds the linearised values
    const QImage lin = run(imageScene(png, 256, 8), Out::Linear);
    for (int x = 0; x < 256; x += 5) {
      const double want = toLinear(Transfer::Srgb, x / 255.0);
      QVERIFY2(std::abs(floatPixel(lin, x, 4)[0] - want) <= 1e-4 + 1e-3 * want, qPrintable(QStringLiteral("x=%1 got %2 want %3").arg(x).arg(floatPixel(lin, x, 4)[0]).arg(want)));
    }
  }

  void transferTagsDecodeLikeTheCpuReference_data() {
    QTest::addColumn<QString>("trc");
    QTest::addColumn<QString>("primaries");
    QTest::addColumn<int>("transfer");
    QTest::addColumn<int>("sdr"); // the compositor's sdrTransfer
    QTest::addColumn<bool>("hdr");
    QTest::newRow("bt709 tag, sRGB-like default") << QStringLiteral("bt709") << QStringLiteral("bt709") << int(Transfer::Srgb) << int(Transfer::Srgb) << false;
    QTest::newRow("bt709 tag, BT.1886") << QStringLiteral("bt709") << QStringLiteral("bt709") << int(Transfer::Bt1886) << int(Transfer::Bt1886) << false;
    QTest::newRow("srgb tag") << QStringLiteral("iec61966-2-1") << QStringLiteral("bt709") << int(Transfer::Srgb) << int(Transfer::Bt1886) << false;
    QTest::newRow("gamma 2.2 tag") << QStringLiteral("bt470m") << QStringLiteral("bt709") << int(Transfer::Gamma22) << int(Transfer::Srgb) << false;
    QTest::newRow("linear tag") << QStringLiteral("linear") << QStringLiteral("bt709") << int(Transfer::Linear) << int(Transfer::Srgb) << false;
    QTest::newRow("PQ") << QStringLiteral("smpte2084") << QStringLiteral("bt2020") << int(Transfer::Pq) << int(Transfer::Srgb) << true;
    QTest::newRow("HLG") << QStringLiteral("arib-std-b67") << QStringLiteral("bt2020") << int(Transfer::Hlg) << int(Transfer::Srgb) << true;
  }

  void transferTagsDecodeLikeTheCpuReference() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    QFETCH(QString, trc);
    QFETCH(QString, primaries);
    QFETCH(int, transfer);
    QFETCH(int, sdr);
    QFETCH(bool, hdr);
    const QString path = rampClip(QStringLiteral("ramp_") + trc, trc, primaries);
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = videoScene(path, 256, 16);
    comp().setSdrTransfer(static_cast<Transfer>(sdr));
    // HDR sources keep their range when the delivery is HDR (no highlight roll-off in the canvas)
    if (hdr) comp().setDelivery(Delivery{Delivery::Encoding::Rec2100Pq, 10, {}});
    RenderStats stats;
    const QImage lin = run(s, Out::Linear, 3, &stats);
    QCOMPARE(stats.layers, 1);
    int worst = 0;
    double worstErr = 0;
    for (int x = 0; x < 256; ++x) {
      const double want = toLinear(static_cast<Transfer>(transfer), x / 255.0);
      const double got = floatPixel(lin, x, 8)[0];
      const double err = std::abs(got - want) - (2e-4 + 2.5e-3 * want);
      if (err > worstErr) {
        worstErr = err;
        worst = x;
      }
      // grey stays grey
      QVERIFY(std::abs(floatPixel(lin, x, 8)[1] - got) <= 1e-3 * (1 + got));
    }
    QVERIFY2(worstErr <= 0, qPrintable(QStringLiteral("worst at x=%1: got %2 want %3").arg(worst).arg(floatPixel(lin, worst, 8)[0]).arg(toLinear(static_cast<Transfer>(transfer), worst / 255.0))));
  }

  // With an SDR delivery the roll-off compresses HDR highlights into 0..1 and leaves everything below the knee alone
  void hdrSourcesAreRolledOffForSdrDelivery() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    const QString path = rampClip(QStringLiteral("ramp_smpte2084"), QStringLiteral("smpte2084"), QStringLiteral("bt2020"));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = videoScene(path, 256, 16);
    const QImage lin = run(s, Out::Linear, 3);
    for (int x = 0; x < 256; ++x) {
      const double raw = toLinear(Transfer::Pq, x / 255.0);
      const double want = toneMapHighlights({raw, raw, raw})[0];
      const double got = floatPixel(lin, x, 8)[0];
      QVERIFY2(got <= 1.0005 && std::abs(got - want) <= 2e-4 + 3e-3 * want, qPrintable(QStringLiteral("x=%1 got %2 want %3").arg(x).arg(got).arg(want)));
    }
  }

  // BT.2020 tagged video is converted to the Rec.709 working primaries: a saturated colour must land where the CPU matrix says
  void wideGamutSourcesAreConverted() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    const QString path = dir_.filePath(QStringLiteral("wide.mkv"));
    QString log;
    // a flat colour tagged BT.2020 primaries / gamma 2.2
    if (!makeClip(path, QStringLiteral("color=c=gray:s=64x64:r=25:d=1,format=gbrp,geq=r='200':g='60':b='20',setparams=color_trc=bt470m:color_primaries=bt2020"),
                  {"-c:v", "ffv1", "-pix_fmt", "gbrp"}, {}, &log))
      QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log)));
    const QImage lin = run(videoScene(path, 64, 64), Out::Linear, 0);
    Rgbd lin2020 = decodeRgb(Transfer::Gamma22, {200 / 255.0, 60 / 255.0, 20 / 255.0});
    Rgbd want = mulMat3(primariesMatrix(Primaries::Rec2020, Primaries::Rec709), lin2020);
    for (double& v : want) v = std::max(v, 0.0);
    const auto got = floatPixel(lin, 32, 32);
    for (size_t k = 0; k < 3; ++k) QVERIFY2(std::abs(got[k] - want[k]) < 6e-3 + 0.02 * want[k], qPrintable(QStringLiteral("channel %1: got %2 want %3").arg(k).arg(got[k]).arg(want[k])));
  }

  void deliveryEncodingsMatchTheCpuReference_data() {
    QTest::addColumn<int>("encoding");
    QTest::addColumn<int>("bits");
    QTest::addColumn<int>("sdr");
    QTest::newRow("sRGB 8") << int(Delivery::Encoding::Srgb) << 8 << int(Transfer::Srgb);
    QTest::newRow("rec709 8 (sRGB-like)") << int(Delivery::Encoding::Rec709) << 8 << int(Transfer::Srgb);
    QTest::newRow("rec709 8 BT.1886") << int(Delivery::Encoding::Rec709) << 8 << int(Transfer::Bt1886);
    QTest::newRow("rec709 10 BT.1886") << int(Delivery::Encoding::Rec709) << 10 << int(Transfer::Bt1886);
    QTest::newRow("rec709 10 BT.709 OETF") << int(Delivery::Encoding::Rec709) << 10 << int(Transfer::Bt709);
    QTest::newRow("rec2100 PQ 10") << int(Delivery::Encoding::Rec2100Pq) << 10 << int(Transfer::Srgb);
    QTest::newRow("rec2100 HLG 10") << int(Delivery::Encoding::Rec2100Hlg) << 10 << int(Transfer::Srgb);
  }

  void deliveryEncodingsMatchTheCpuReference() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    QFETCH(int, encoding);
    QFETCH(int, bits);
    QFETCH(int, sdr);
    const auto enc = static_cast<Delivery::Encoding>(encoding);
    const std::vector<Rgb8> colours = sampleColours();
    const QString png = savePng(QStringLiteral("palette"), palette(colours));
    const Scene s = imageScene(png, 16, 4);
    comp().setSdrTransfer(static_cast<Transfer>(sdr));
    comp().setDelivery(Delivery{enc, bits, {}});
    const QImage img = run(s, Out::Rgba64);
    QCOMPARE(img.format(), QImage::Format_RGBA64);
    const int maxCode = bits > 8 ? 1023 : 255;
    Transfer out = Transfer::Srgb;
    Primaries prim = Primaries::Rec709;
    if (enc == Delivery::Encoding::Rec709) out = static_cast<Transfer>(sdr);
    if (enc == Delivery::Encoding::Rec2100Pq) out = Transfer::Pq, prim = Primaries::Rec2020;
    if (enc == Delivery::Encoding::Rec2100Hlg) out = Transfer::Hlg, prim = Primaries::Rec2020;
    int worst = 0;
    for (size_t i = 0; i < colours.size(); ++i) {
      const Rgb8 c = colours[i];
      const Rgbd lin = {expectedLin(Transfer::Srgb, c.r), expectedLin(Transfer::Srgb, c.g), expectedLin(Transfer::Srgb, c.b)};
      Rgbd e = encodeRgb(out, mulMat3(primariesMatrix(Primaries::Rec709, prim), lin));
      const auto got = codes(img, static_cast<int>(i), 2, maxCode);
      for (size_t k = 0; k < 3; ++k) {
        const int want = static_cast<int>(std::lround(std::clamp(e[k], 0.0, 1.0) * maxCode));
        worst = std::max(worst, std::abs(got[k] - want));
      }
    }
    QVERIFY2(worst <= 1, qPrintable(QStringLiteral("worst code error %1 of %2").arg(worst).arg(maxCode)));
  }

  // ---- blending ----

  void linearAndDisplayBlending() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    const QString grey = savePng(QStringLiteral("grey"), palette({{128, 128, 128}}));
    const QString red = savePng(QStringLiteral("red"), palette({{255, 0, 0}}));
    const QString green = savePng(QStringLiteral("green"), palette({{0, 255, 0}}));
    const QString white = savePng(QStringLiteral("white"), palette({{255, 255, 255}}));
    const auto twoLayers = [&](const QString& bottom, const QString& top, double opacity, BlendSpace space) {
      Scene s;
      s.doc = newDoc(1, 4);
      s.assets["ast_b"] = {bottom, AssetKind::Image};
      s.assets["ast_t"] = {top, AssetKind::Image};
      const QString lower = mainTrack(s.doc);
      const QString upper = addTopTrack(s.doc, QStringLiteral("trk_top"));
      addMedia(s.doc, ItemType::Image, lower, QStringLiteral("itm_b"), QStringLiteral("ast_b"), 0, 10);
      addMedia(s.doc, ItemType::Image, upper, QStringLiteral("itm_t"), QStringLiteral("ast_t"), 0, 10).transform.opacity = opacity;
      setColor(s, [&](ColorManagement& c) { c.blendSpace = space; });
      return s;
    };
    // 50% grey (128) over black. Display: 64, the reference. Linear: sRGB(0.5 * lin(128)) = 92.
    {
      const QImage d = run(twoLayers(white, grey, 0.5, BlendSpace::Display), Out::Rgba8); // over white: 191.5
      SF_VERIFY_PIXEL(d, 0, 2, (Rgb{191, 191, 191}), 1);
      Scene s = imageScene(grey, 1, 4);
      findItem(s.doc, QStringLiteral("itm_i")).transform.opacity = 0.5;
      setColor(s, [](ColorManagement& c) { c.blendSpace = BlendSpace::Display; });
      SF_VERIFY_PIXEL(run(s, Out::Rgba8), 0, 2, (Rgb{64, 64, 64}), 1);
      setColor(s, [](ColorManagement& c) { c.blendSpace = BlendSpace::Linear; });
      SF_VERIFY_PIXEL(run(s, Out::Rgba8), 0, 2, (Rgb{92, 92, 92}), 1);
      // no colour management block at all: linear
      s.doc.project.colorManagement.reset();
      SF_VERIFY_PIXEL(run(s, Out::Rgba8), 0, 2, (Rgb{92, 92, 92}), 1);
      // the compositor's option beats the project's
      comp().setBlendSpace(BlendSpace::Display);
      SF_VERIFY_PIXEL(run(s, Out::Rgba8), 0, 2, (Rgb{64, 64, 64}), 1);
      comp().setBlendSpace(std::nullopt);
    }
    // 50% red over green: Display (128,128,0), Linear (188,188,0)
    SF_VERIFY_PIXEL(run(twoLayers(green, red, 0.5, BlendSpace::Display), Out::Rgba8), 0, 2, (Rgb{128, 128, 0}), 1);
    SF_VERIFY_PIXEL(run(twoLayers(green, red, 0.5, BlendSpace::Linear), Out::Rgba8), 0, 2, (Rgb{188, 188, 0}), 1);
    // opaque layers don't care
    SF_VERIFY_PIXEL(run(twoLayers(green, red, 1.0, BlendSpace::Linear), Out::Rgba8), 0, 2, (Rgb{255, 0, 0}), 0);
    // the float canvas shows the difference directly: 0.5 linear vs the encoded 0.5 * 1
    const auto lin = floatPixel(run(twoLayers(green, red, 0.5, BlendSpace::Linear), Out::Linear), 0, 2);
    QVERIFY(std::abs(lin[0] - 0.5f) < 1e-3f && std::abs(lin[1] - 0.5f) < 1e-3f && std::abs(lin[2]) < 1e-4f);
    const auto enc = floatPixel(run(twoLayers(green, red, 0.5, BlendSpace::Display), Out::Linear), 0, 2);
    QVERIFY(std::abs(enc[0] - 0.5f) < 1e-3f && std::abs(enc[1] - 0.5f) < 1e-3f);
  }

  // Colour effects are display-referred maths (the reference's): the same result in both blend spaces
  void colourEffectsAreBlendSpaceIndependent() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    const QString png = savePng(QStringLiteral("pal"), palette(sampleColours()));
    Scene s = imageScene(png, 16, 4);
    Effect e;
    e.id = QStringLiteral("fx_1");
    e.type = QStringLiteral("contrast");
    e.params[QStringLiteral("amount")] = 1.4;
    findItem(s.doc, QStringLiteral("itm_i")).effects.push_back(e);
    setColor(s, [](ColorManagement& c) { c.blendSpace = BlendSpace::Display; });
    const QImage a = run(s, Out::Rgba8);
    setColor(s, [](ColorManagement& c) { c.blendSpace = BlendSpace::Linear; });
    const QImage b = run(s, Out::Rgba8);
    QVERIFY(maxAbsDiff(a, b) <= 1);
    // and they are the CSS arithmetic on the encoded values
    const std::vector<Rgb8> cols = sampleColours();
    for (size_t i = 0; i < cols.size(); ++i) {
      const int want = to8(1.4 * (cols[i].r / 255.0) + (0.5 - 0.7));
      QVERIFY2(std::abs(qRed(a.pixel(static_cast<int>(i), 2)) - want) <= 1, qPrintable(QString::number(i)));
    }
  }

  // ---- 10-bit sources ----

  // A narrow luma ramp: a 10-bit source has four times the steps of an 8-bit one. They must survive the
  // trip through the float canvas to a 10-bit delivery (no banding added), and the 8-bit source must not gain steps.
  void tenBitSourceStaysSmooth() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (!comp().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    const QString p10 = dir_.filePath(QStringLiteral("ramp10.mp4"));
    const QString p8 = dir_.filePath(QStringLiteral("ramp8.mp4"));
    QString log;
    // luma 10-bit 400..527 over 640 px (5 px per code), the same ramp quantised to 8 bits: 100..131 (20 px per code).
    // 640x360: the hardware decoders have a minimum picture size.
    const QString g10 = QStringLiteral("color=c=gray:s=640x360:r=25:d=1,format=gray10le,geq=lum='400+floor(X/5)',format=p010le");
    if (!makeClip(p10, g10, {"-c:v", "hevc_nvenc", "-profile:v", "main10", "-b:v", "40M", "-g", "10", "-bf", "0"}, {}, &log))
      QSKIP(qPrintable(QStringLiteral("cannot encode 10-bit hevc: %1").arg(log.left(200))));
    const QString g8 = QStringLiteral("color=c=gray:s=640x360:r=25:d=1,format=gray,geq=lum='100+floor(X/20)',format=yuv420p");
    if (!makeClip(p8, g8, {"-c:v", "hevc_nvenc", "-b:v", "40M", "-g", "10", "-bf", "0"}, {}, &log)) QSKIP(qPrintable(QStringLiteral("cannot encode 8-bit hevc: %1").arg(log.left(200))));

    const auto levels = [&](const QString& path, std::set<int>* distinct, QString* decoder) {
      Scene s = videoScene(path, 640, 360);
      FrameService::Options o;
      o.hw = HwMode::Required;
      o.gpuFrames = true;
      o.cacheBytes = 64ll << 20;
      FrameService svc(o);
      FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
      comp().setGpuEnabled(true);
      comp().setDelivery(Delivery{Delivery::Encoding::Srgb, 10, {}});
      RenderStats stats;
      const QImage img = gpu_->renderRgba64(s.doc, 0, prov, &stats);
      *decoder = svc.decoderName(1);
      if (stats.gpuLayers != 1) return false;
      int prev = -1, steps = 0;
      for (int x = 0; x < 640; ++x) {
        const int v = codes(img, x, 180, 1023)[0];
        distinct->insert(v);
        if (v != prev) ++steps;
        prev = v;
      }
      Q_UNUSED(steps);
      return true;
    };
    std::set<int> d10, d8;
    QString dec10, dec8;
    if (!levels(p10, &d10, &dec10) || !levels(p8, &d8, &dec8)) QSKIP(qPrintable(QStringLiteral("zero-copy not available: %1 / %2").arg(dec10, dec8)));
    qInfo() << "distinct 10-bit output levels along the ramp: 10-bit source" << d10.size() << "vs 8-bit source" << d8.size();
    QVERIFY2(d8.size() >= 20 && d8.size() <= 60, qPrintable(QString::number(d8.size())));
    QVERIFY2(d10.size() >= 2 * d8.size(), qPrintable(QStringLiteral("10-bit ramp has %1 levels, 8-bit has %2").arg(d10.size()).arg(d8.size())));
    QVERIFY2(d10.size() >= 90, qPrintable(QString::number(d10.size())));
  }

  // The stream's PQ tag must reach the YUV (zero-copy) path too: its canvas agrees with the software decode's
  void hdrTagsReachTheGpuPath() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (!comp().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    if (ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
    const QString path = dir_.filePath(QStringLiteral("pq10.mp4"));
    QString log;
    // limited-range 10-bit luma 64..940 across 640 px, tagged PQ / BT.2020
    const QString g = QStringLiteral("color=c=gray:s=640x360:r=25:d=1,format=yuv420p10le,geq=lum='64+floor(X*876/640)':cb=512:cr=512,format=p010le,"
                                     "setparams=color_trc=smpte2084:color_primaries=bt2020:colorspace=bt2020nc:range=tv");
    if (!makeClip(path, g, {"-c:v", "hevc_nvenc", "-profile:v", "main10", "-b:v", "40M", "-g", "10", "-bf", "0"}, {}, &log))
      QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log.left(200))));
    const Scene s = videoScene(path, 640, 360);
    comp().setDelivery(Delivery{Delivery::Encoding::Rec2100Pq, 10, {}}); // keep the HDR range in the canvas
    FrameService::Options o;
    o.hw = HwMode::Required;
    o.gpuFrames = true;
    o.cacheBytes = 64ll << 20;
    QImage viaGpu;
    {
      FrameService svc(o);
      FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
      comp().setGpuEnabled(true);
      RenderStats stats;
      viaGpu = gpu_->renderLinear(s.doc, 0, prov, &stats);
      if (stats.gpuLayers != 1) QSKIP("zero-copy not available");
    }
    const QImage viaSw = run(s, Out::Linear);
    for (const int x : {60, 160, 320, 480, 580}) {
      const double code = (64 + std::floor(x * 876.0 / 640.0) - 64) / 876.0;
      const double want = toLinear(Transfer::Pq, code);
      const double gpu = floatPixel(viaGpu, x, 180)[0], sw = floatPixel(viaSw, x, 180)[0];
      QVERIFY2(std::abs(gpu - want) <= 0.04 * want + 2e-3, qPrintable(QStringLiteral("x=%1 gpu %2 want %3").arg(x).arg(gpu).arg(want)));
      QVERIFY2(std::abs(gpu - sw) <= 0.08 * want + 4e-3, qPrintable(QStringLiteral("x=%1 gpu %2 sw %3").arg(x).arg(gpu).arg(sw)));
    }
  }

  // ---- OpenColorIO ----

  void ocioConfigLists() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    QVERIFY2(cm_, qPrintable(cmError_));
    qInfo() << "OpenColorIO" << ColorManager::version() << "config:" << cm_->description().left(80);
    const QStringList spaces = cm_->colorSpaces();
    QVERIFY(spaces.size() > 20);
    QVERIFY(cm_->hasColorSpace(QStringLiteral("ACEScg")));
    QVERIFY(!cm_->displays().isEmpty());
    const QString display = cm_->defaultDisplay();
    QVERIFY(!display.isEmpty());
    QVERIFY(!cm_->views(display).isEmpty());
    QVERIFY(!cm_->defaultView(display).isEmpty());
    qInfo() << "displays" << cm_->displays() << "default view" << cm_->defaultView(display) << "looks" << cm_->looks();
    QCOMPARE(cm_->linearRec709Space(), QStringLiteral("Linear Rec.709 (sRGB)"));
    for (const QRegularExpression& re : {QRegularExpression(QStringLiteral("LogC")), QRegularExpression(QStringLiteral("S-Log3")), QRegularExpression(QStringLiteral("V-Log"))})
      qInfo() << re.pattern() << "->" << findSpace(re);
    QString err;
    QVERIFY(!ColorManager::create(dir_.filePath(QStringLiteral("nope.ocio")), &err));
    QVERIFY(!err.isEmpty());
  }

  // Identity transforms never become LUTs: the shader skips them, so the round trip is exact
  void ocioIdentityConfigRoundTrip() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    const QString cfgPath = dir_.filePath(QStringLiteral("identity.ocio"));
    {
      QFile f(cfgPath);
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
      QTextStream(&f) << "ocio_profile_version: 2\n"
                         "name: identity\n"
                         "roles:\n  default: Linear Rec.709 (sRGB)\n  scene_linear: Linear Rec.709 (sRGB)\n"
                         "displays:\n  Raw:\n    - !<View> {name: Raw, colorspace: Same As Linear}\n"
                         "colorspaces:\n"
                         "  - !<ColorSpace>\n    name: Linear Rec.709 (sRGB)\n    isdata: false\n"
                         "  - !<ColorSpace>\n    name: Same As Linear\n    isdata: false\n    to_scene_reference: !<MatrixTransform> {matrix: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]}\n";
    }
    QString err;
    std::shared_ptr<ColorManager> id = ColorManager::create(cfgPath, &err);
    QVERIFY2(id, qPrintable(err));
    // CPU: unchanged bit for bit
    float px[6] = {0.0f, 0.25f, 1.0f, 0.123456f, 0.5f, 0.999f};
    float orig[6];
    std::copy(px, px + 6, orig);
    QVERIFY(id->applyInputCpu({QStringLiteral("Same As Linear"), {}, 1, {}}, px, 2, &err));
    for (int i = 0; i < 6; ++i) QCOMPARE(px[i], orig[i]);
    QCOMPARE(id->inputTransform({QStringLiteral("Same As Linear"), {}, 1, {}}).kind, GpuTransform::Kind::Passthrough);
    QCOMPARE(id->displayTransform({{}, QStringLiteral("Raw"), QStringLiteral("Raw"), {}, {}}).kind, GpuTransform::Kind::Passthrough);

    // GPU: an sRGB ramp through the display transform "identity" comes out as its linear value, within 1/1023
    const QString png = savePng(QStringLiteral("ramp"), greyRamp());
    Scene s = imageScene(png, 256, 8);
    setColor(s, [](ColorManagement& c) { c.outputSpace = QStringLiteral("Same As Linear"); });
    comp().setColorManager(id);
    comp().setDelivery(Delivery{Delivery::Encoding::Srgb, 10, QStringLiteral("Same As Linear")});
    RenderStats stats;
    const QImage img = run(s, Out::Rgba64, 0, &stats);
    QCOMPARE(stats.colorErrors, 0);
    int worst = 0;
    for (int x = 0; x < 256; ++x) {
      const int want = static_cast<int>(std::lround(toLinear(Transfer::Srgb, x / 255.0) * 1023.0));
      worst = std::max(worst, std::abs(codes(img, x, 4, 1023)[0] - want));
    }
    QVERIFY2(worst <= 1, qPrintable(QStringLiteral("worst error %1/1023").arg(worst)));
  }

  void ocioLutFileAppliesAsALook() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    QVERIFY2(cm_, qPrintable(cmError_));
    // a channel-rotating, offset 17^3 cube: out = (0.1 + 0.8 g, 0.1 + 0.8 b, 0.1 + 0.8 r). Linear in its input, so
    // every interpolation along the way is exact and the expected pixels follow from the file alone.
    const QString cube = writeCube(QStringLiteral("rot.cube"), 17, [](double r, double g, double b) { return std::array<double, 3>{0.1 + 0.8 * g, 0.1 + 0.8 * b, 0.1 + 0.8 * r}; });
    QVERIFY(!cube.isEmpty());
    const std::vector<Rgb8> cols = sampleColours();
    const QString png = savePng(QStringLiteral("pal"), palette(cols));
    Scene s = imageScene(png, 16, 4);
    comp().setColorManager(cm_);
    for (const double intensity : {1.0, 0.5}) {
      ItemColor ic;
      ic.lutPath = cube;
      ic.lutIntensity = intensity;
      findItem(s.doc, QStringLiteral("itm_i")).color = ic;
      RenderStats stats;
      const QImage img = run(s, Out::Rgba8, 0, &stats);
      QCOMPARE(stats.colorErrors, 0);
      int worst = 0;
      for (size_t i = 0; i < cols.size(); ++i) {
        const double in[3] = {cols[i].r / 255.0, cols[i].g / 255.0, cols[i].b / 255.0};
        const double lut[3] = {0.1 + 0.8 * in[1], 0.1 + 0.8 * in[2], 0.1 + 0.8 * in[0]};
        const QRgb px = img.pixel(static_cast<int>(i), 2);
        const int got[3] = {qRed(px), qGreen(px), qBlue(px)};
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(got[k] - to8(in[k] + (lut[k] - in[k]) * intensity)));
      }
      QVERIFY2(worst <= 2, qPrintable(QStringLiteral("intensity %1: worst error %2").arg(intensity).arg(worst)));
    }
    // a non-linear cube against OCIO's own CPU evaluation (tetrahedral interpolation of the same file)
    const QString gam = writeCube(QStringLiteral("gamma.cube"), 17, [](double r, double g, double b) { return std::array<double, 3>{r * r, std::pow(g, 0.6), b * b * b}; });
    ItemColor ic;
    ic.lutPath = gam;
    findItem(s.doc, QStringLiteral("itm_i")).color = ic;
    const QImage img = run(s, Out::Rgba8);
    std::vector<float> ref;
    for (const Rgb8& c : cols) ref.insert(ref.end(), {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f});
    QString err;
    QVERIFY2(cm_->applyInputCpu({{}, gam, 1, {}}, ref.data(), cols.size(), &err), qPrintable(err));
    int worst = 0;
    for (size_t i = 0; i < cols.size(); ++i) {
      // the CPU result is linear light after the built-in sRGB decode; the delivery encodes it again
      const QRgb px = img.pixel(static_cast<int>(i), 2);
      const int got[3] = {qRed(px), qGreen(px), qBlue(px)};
      for (size_t k = 0; k < 3; ++k) worst = std::max(worst, std::abs(got[k] - to8(fromLinear(Transfer::Srgb, ref[i * 3 + k]))));
    }
    QVERIFY2(worst <= 3, qPrintable(QStringLiteral("gamma cube vs OCIO CPU: worst error %1").arg(worst)));
  }

  void ocioLogInputMatchesTheCpuProcessor_data() {
    QTest::addColumn<QString>("pattern");
    QTest::newRow("ARRI LogC3") << QStringLiteral("^ARRI LogC3 \\(EI800\\)");
    QTest::newRow("S-Log3") << QStringLiteral("^S-Log3 S-Gamut3$");
    QTest::newRow("V-Log") << QStringLiteral("^V-Log V-Gamut$");
    QTest::newRow("Rec.709 display-referred input") << QStringLiteral("^Rec\\.1886 Rec\\.709 - Display$");
  }

  void ocioLogInputMatchesTheCpuProcessor() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    QVERIFY2(cm_, qPrintable(cmError_));
    QFETCH(QString, pattern);
    const QString space = findSpace(QRegularExpression(pattern));
    if (space.isEmpty()) QSKIP(qPrintable(QStringLiteral("no colour space matching %1 in the config").arg(pattern)));
    const std::vector<Rgb8> cols = sampleColours();
    const QString png = savePng(QStringLiteral("pal"), palette(cols));
    Scene s = imageScene(png, 16, 4);
    ItemColor ic;
    ic.inputSpace = space;
    findItem(s.doc, QStringLiteral("itm_i")).color = ic;
    comp().setColorManager(cm_);
    RenderStats stats;
    const QImage lin = run(s, Out::Linear, 0, &stats);
    QCOMPARE(stats.colorErrors, 0);
    std::vector<float> ref;
    for (const Rgb8& c : cols) ref.insert(ref.end(), {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f});
    QString err;
    QVERIFY2(cm_->applyInputCpu({space, {}, 1, {}}, ref.data(), cols.size(), &err), qPrintable(err));
    double worst = 0;
    for (size_t i = 0; i < cols.size(); ++i) {
      const auto got = floatPixel(lin, static_cast<int>(i), 2);
      for (size_t k = 0; k < 3; ++k) {
        const double want = std::max(0.0f, ref[i * 3 + k]);
        // the same transform sampled from a 65^3 cube: a fraction of an 8-bit step in the encoded domain
        const double err8 = std::abs(fromLinear(Transfer::Srgb, got[k]) - fromLinear(Transfer::Srgb, want)) * 255.0;
        worst = std::max(worst, err8);
      }
    }
    qInfo() << space << "worst difference from the CPU processor (8-bit sRGB steps)" << worst;
    // a 65^3 cube of a log curve plus a gamut matrix: up to about two 8-bit steps on saturated colours
    QVERIFY2(worst < 2.0, qPrintable(QString::number(worst)));
  }

  // mid grey in a camera log curve is 18% scene linear: an independent check of the CPU path
  void ocioLogCMidGrey() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    QVERIFY2(cm_, qPrintable(cmError_));
    const QString space = findSpace(QRegularExpression(QStringLiteral("^ARRI LogC3 \\(EI800\\)")));
    if (space.isEmpty()) QSKIP("no LogC3 space");
    // LogC3 EI800: t = 0.0105909 ... 18% grey encodes to 0.391007
    float px[3] = {0.391007f, 0.391007f, 0.391007f};
    QString err;
    QVERIFY2(cm_->applyInputCpu({space, {}, 1, {}}, px, 1, &err), qPrintable(err));
    for (float v : px) QVERIFY2(std::abs(v - 0.18f) < 2e-3f, qPrintable(QString::number(v)));
  }

  void ocioMissingFilesFailCleanly() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    QVERIFY2(cm_, qPrintable(cmError_));
    const QString missing = dir_.filePath(QStringLiteral("not_here.cube"));
    const GpuTransform t = cm_->inputTransform({{}, missing, 1, {}});
    QCOMPARE(t.kind, GpuTransform::Kind::Failed);
    QVERIFY2(t.error.contains(QStringLiteral("not found")), qPrintable(t.error));
    const GpuTransform badSpace = cm_->inputTransform({QStringLiteral("No Such Space"), {}, 1, {}});
    QCOMPARE(badSpace.kind, GpuTransform::Kind::Failed);
    QVERIFY(badSpace.error.contains(QStringLiteral("No Such Space")));
    // a file that exists but isn't a LUT
    const QString junk = dir_.filePath(QStringLiteral("junk.cube"));
    {
      QFile f(junk);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("this is not a LUT\n");
    }
    QCOMPARE(cm_->inputTransform({{}, junk, 1, {}}).kind, GpuTransform::Kind::Failed);

    // the compositor still draws the layer (built-in colour) and reports it
    const QString png = savePng(QStringLiteral("pal"), palette(sampleColours()));
    Scene s = imageScene(png, 16, 4);
    ItemColor ic;
    ic.lutPath = missing;
    findItem(s.doc, QStringLiteral("itm_i")).color = ic;
    comp().setColorManager(cm_);
    RenderStats stats;
    const QImage img = run(s, Out::Rgba8, 0, &stats);
    QCOMPARE(stats.layers, 1);
    QCOMPARE(stats.colorErrors, 1);
    QVERIFY2(comp().lastColorError().contains(QStringLiteral("not found")), qPrintable(comp().lastColorError()));
    SF_VERIFY_PIXEL(img, 0, 2, (Rgb{255, 0, 0}), 1);
    // without a manager an item that asks for OCIO is reported the same way
    comp().setColorManager(nullptr);
    RenderStats stats2;
    run(s, Out::Rgba8, 0, &stats2);
    QCOMPARE(stats2.colorErrors, 1);
    // and a clean frame clears the message
    findItem(s.doc, QStringLiteral("itm_i")).color.reset();
    run(s, Out::Rgba8, 0, &stats2);
    QCOMPARE(stats2.colorErrors, 0);
    QVERIFY(comp().lastColorError().isEmpty());
  }

  void ocioWorkingSpaceIsConfigurable() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    QVERIFY2(cm_, qPrintable(cmError_));
    const std::vector<Rgb8> cols = sampleColours();
    const QString png = savePng(QStringLiteral("pal"), palette(cols));
    Scene s = imageScene(png, 16, 4);
    setColor(s, [](ColorManagement& c) { c.workingSpace = QStringLiteral("ACEScg"); });
    comp().setColorManager(cm_);
    RenderStats stats;
    const QImage lin = run(s, Out::Linear, 0, &stats);
    QCOMPARE(stats.colorErrors, 0);
    // red (255,0,0) in ACEScg: the first column of the Rec.709 -> AP1 matrix
    const auto red = floatPixel(lin, 0, 2);
    QVERIFY2(std::abs(red[0] - 0.6131f) < 3e-3f && std::abs(red[1] - 0.0702f) < 3e-3f && std::abs(red[2] - 0.0206f) < 3e-3f,
             qPrintable(QStringLiteral("(%1, %2, %3)").arg(red[0]).arg(red[1]).arg(red[2])));
    // the built-in sRGB delivery converts back: the picture is unchanged
    const QImage out = run(s, Out::Rgba8);
    QVERIFY2(maxAbsDiff(out, run(imageScene(png, 16, 4), Out::Rgba8)) <= 1, qPrintable(QString::number(maxAbsDiff(out, run(imageScene(png, 16, 4), Out::Rgba8)))));
    // a working space the config doesn't have is reported and ignored
    setColor(s, [](ColorManagement& c) { c.workingSpace = QStringLiteral("Not A Space"); });
    RenderStats bad;
    run(s, Out::Rgba8, 0, &bad);
    QCOMPARE(bad.colorErrors, 1);
  }

  // The display transform baked into a 65^3 LUT through the log shaper against OCIO's CPU processor
  void ocioDisplayTransformMatchesTheCpuProcessor() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    if (!gpu_) QSKIP(qPrintable(skip_));
    QVERIFY2(cm_, qPrintable(cmError_));
    const QString display = cm_->defaultDisplay();
    const QString view = cm_->defaultView(display);
    const std::vector<Rgb8> cols = sampleColours();
    const QString png = savePng(QStringLiteral("pal"), palette(cols));
    Scene s = imageScene(png, 16, 4);
    setColor(s, [&](ColorManagement& c) { c.displayView = display + QLatin1Char('/') + view; });
    comp().setColorManager(cm_);
    const QImage shown = preview(s); // present() through the project's display / view
    QVERIFY(!shown.isNull());
    std::vector<float> ref;
    for (const Rgb8& c : cols) ref.insert(ref.end(), {float(expectedLin(Transfer::Srgb, c.r)), float(expectedLin(Transfer::Srgb, c.g)), float(expectedLin(Transfer::Srgb, c.b))});
    QString err;
    QVERIFY2(cm_->applyDisplayCpu({{}, display, view, {}, {}}, ref.data(), cols.size(), &err), qPrintable(err));
    int worst = 0;
    for (size_t i = 0; i < cols.size(); ++i) {
      const QRgb px = shown.pixel(static_cast<int>(i), 2);
      const int got[3] = {qRed(px), qGreen(px), qBlue(px)};
      for (size_t k = 0; k < 3; ++k) worst = std::max(worst, std::abs(got[k] - to8(ref[i * 3 + k])));
    }
    qInfo() << "preview through" << display << "/" << view << ": worst difference from the CPU processor" << worst << "of 255";
    QVERIFY2(worst <= 3, qPrintable(QString::number(worst)));
    // without a display view the preview is the plain sRGB encode: identical to the 8-bit delivery
    Scene plain = imageScene(png, 16, 4);
    QVERIFY(maxAbsDiff(preview(plain), run(plain, Out::Rgba8)) <= 1);

    // delivery through a display-referred colour space of the config
    const QString outSpace = display;
    if (cm_->hasColorSpace(outSpace)) {
      Scene d = imageScene(png, 16, 4);
      setColor(d, [&](ColorManagement& c) { c.outputSpace = outSpace; });
      RenderStats stats;
      const QImage img = run(d, Out::Rgba8, 0, &stats);
      QCOMPARE(stats.colorErrors, 0);
      std::vector<float> ref2;
      for (const Rgb8& c : cols) ref2.insert(ref2.end(), {float(expectedLin(Transfer::Srgb, c.r)), float(expectedLin(Transfer::Srgb, c.g)), float(expectedLin(Transfer::Srgb, c.b))});
      QVERIFY2(cm_->applyDisplayCpu({{}, {}, {}, {}, outSpace}, ref2.data(), cols.size(), &err), qPrintable(err));
      int worst2 = 0;
      for (size_t i = 0; i < cols.size(); ++i) {
        const QRgb px = img.pixel(static_cast<int>(i), 2);
        const int got[3] = {qRed(px), qGreen(px), qBlue(px)};
        for (size_t k = 0; k < 3; ++k) worst2 = std::max(worst2, std::abs(got[k] - to8(ref2[i * 3 + k])));
      }
      QVERIFY2(worst2 <= 3, qPrintable(QString::number(worst2)));
    }
  }

  void ocioProcessorsAreCached() {
    if (!ColorManager::available()) QSKIP("built without OpenColorIO");
    QVERIFY2(cm_, qPrintable(cmError_));
    const QString space = findSpace(QRegularExpression(QStringLiteral("^ARRI LogC3 \\(EI800\\)")));
    if (space.isEmpty()) QSKIP("no LogC3 space");
    const GpuTransform a = cm_->inputTransform({space, {}, 1, {}});
    const GpuTransform b = cm_->inputTransform({space, {}, 1, {}});
    QCOMPARE(a.kind, GpuTransform::Kind::Lut);
    QVERIFY(a.lut && a.lut == b.lut); // the same baked cube
    QCOMPARE(a.lut->size, ColorManager::kLutSize);
    QCOMPARE(a.lut->rgba.size(), static_cast<size_t>(ColorManager::kLutSize) * ColorManager::kLutSize * ColorManager::kLutSize * 4);
    const GpuTransform c = cm_->inputTransform({space, {}, 1, QStringLiteral("ACEScg")});
    QVERIFY(c.lut && c.lut != a.lut);
  }
};

QTEST_MAIN(TstColor)
#include "tst_color.moc"
