// Offscreen compositor tests: documents in, pixels out. Clips are generated with the ffmpeg CLI like in
// the media tests; QRhi runs on D3D11 without a window. If no D3D11 device can be created the whole
// suite skips with a message instead of failing.

#include "core/apply.h"
#include "media/probe.h"
#include "render/offscreen.h"
#include "render_testutil.h"

#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::render;
using namespace sf::test;

class TstCompositor : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  std::unique_ptr<OffscreenRenderer> gpu_;
  QString skip_;
  QHash<QString, QString> clips_;

  QString clip(const QString& name, const QString& lavfi, const QStringList& codec = h264(), const QStringList& before = {}) {
    if (const auto it = clips_.find(name); it != clips_.end()) return it.value();
    const QString path = dir_.filePath(name + QStringLiteral(".mp4"));
    QString log;
    const bool ok = makeClip(path, lavfi, codec, before, &log);
    if (!ok) skip_ = QStringLiteral("cannot encode %1: %2").arg(name, log.left(200));
    clips_.insert(name, ok ? path : QString());
    return clips_.value(name);
  }

  struct Scene {
    TimelineDoc doc;
    AssetTable assets;
  };

  // One main-track clip filling [0, frames) of a canvas the size of the clip.
  Scene oneClip(const QString& path, int w, int h, Frame frames = 40) {
    Scene s;
    s.doc = newDoc(w, h);
    s.assets["ast_a"] = {path, AssetKind::Video};
    addMedia(s.doc, ItemType::Video, mainTrack(s.doc), QStringLiteral("itm_a"), QStringLiteral("ast_a"), 0, frames);
    return s;
  }

  QImage renderWith(const Scene& s, Frame frame, bool gpu, HwMode hw, RenderStats* stats = nullptr, QString* decoder = nullptr) {
    FrameService::Options o;
    o.hw = hw;
    o.gpuFrames = gpu;
    o.cacheBytes = 128ll << 20;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    gpu_->compositor().setGpuEnabled(gpu);
    const QImage img = gpu_->render(s.doc, frame, prov, stats);
    if (decoder) {
      for (AssetId id = 1; id < 4 && decoder->isEmpty(); ++id) *decoder = svc.decoderName(id);
    }
    return img;
  }

  QImage render(const Scene& s, Frame frame, RenderStats* stats = nullptr) { return renderWith(s, frame, false, HwMode::Off, stats); }

  // The reference blends in gamma; the native default is linear light. Tests of overlaps say which.
  static void blendIn(Scene& s, BlendSpace space) {
    ColorManagement cm = s.doc.project.colorManagement.value_or(ColorManagement{});
    cm.blendSpace = space;
    s.doc.project.colorManagement = cm;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    QString err;
    gpu_ = OffscreenRenderer::create(&err);
    if (!gpu_) skip_ = QStringLiteral("no D3D11 QRhi: %1").arg(err);
  }

  void init() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    if (sf::test::ffmpegExe().isEmpty()) QSKIP("SF_FFMPEG_EXE not set");
  }

  void solidClipFillsTheCanvas() {
    const QString path = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 320, 240);
    RenderStats stats;
    const QImage img = render(s, 5, &stats);
    QCOMPARE(img.size(), QSize(320, 240));
    QCOMPARE(stats.layers, 1);
    SF_VERIFY_PIXEL(img, 0, 0, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(img, 160, 120, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(img, 319, 239, (Rgb{255, 0, 0}), 4);
  }

  void canvasSizeComesFromTheDocument() {
    const QString path = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 640, 360);
    const QImage img = render(s, 0);
    QCOMPARE(img.size(), QSize(640, 360));
  }

  // The reference fits media into the canvas ("contain"): a 4:3 clip in a 16:9 canvas gets side bars
  // filled with the project's background colour.
  void fitContainLeavesBackgroundBars() {
    const QString path = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(path, 640, 360);
    s.doc.project.styleConfig.backgroundColor = QStringLiteral("#102030");
    const QImage img = render(s, 0);
    // 4:3 in 16:9 -> 480x360 centred, bars of 80 px
    SF_VERIFY_PIXEL(img, 40, 180, (Rgb{0x10, 0x20, 0x30}), 1);
    SF_VERIFY_PIXEL(img, 600, 180, (Rgb{0x10, 0x20, 0x30}), 1);
    SF_VERIFY_PIXEL(img, 100, 180, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(img, 559, 5, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(img, 561, 5, (Rgb{0x10, 0x20, 0x30}), 1);
    SF_VERIFY_PIXEL(img, 81, 355, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(img, 78, 355, (Rgb{0x10, 0x20, 0x30}), 1);
  }

  // Orientation: no flips, quadrants where they belong.
  void quadrantsAreUpright() {
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 320, 240);
    const QImage img = render(s, 3);
    SF_VERIFY_PIXEL(img, 80, 60, (Rgb{255, 0, 0}), 6);
    SF_VERIFY_PIXEL(img, 240, 60, (Rgb{0, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 80, 180, (Rgb{0, 0, 255}), 6);
    SF_VERIFY_PIXEL(img, 240, 180, (Rgb{255, 255, 0}), 6);
  }

  void transformScaleAndPosition() {
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(path, 320, 240);
    Item& item = findItem(s.doc, QStringLiteral("itm_a"));
    item.transform.scale = 0.5;
    item.transform.x = 40;
    item.transform.y = -20;
    const QImage img = render(s, 0);
    // quarter-size picture centred at (200, 100): spans x 120..280, y 40..160
    SF_VERIFY_PIXEL(img, 130, 50, (Rgb{255, 0, 0}), 6);
    SF_VERIFY_PIXEL(img, 270, 50, (Rgb{0, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 130, 150, (Rgb{0, 0, 255}), 6);
    SF_VERIFY_PIXEL(img, 270, 150, (Rgb{255, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 100, 100, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 200, 20, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 200, 180, (Rgb{0, 0, 0}), 1);
  }

  void nonUniformScale() {
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(path, 320, 240);
    Item& item = findItem(s.doc, QStringLiteral("itm_a"));
    item.transform.scaleX = 0.5; // half as wide, full height
    const QImage img = render(s, 0);
    SF_VERIFY_PIXEL(img, 90, 60, (Rgb{255, 0, 0}), 6);
    SF_VERIFY_PIXEL(img, 230, 60, (Rgb{0, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 40, 60, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 280, 60, (Rgb{0, 0, 0}), 1);
  }

  void transformRotation() {
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(320, 240));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(path, 320, 240);
    findItem(s.doc, QStringLiteral("itm_a")).transform.rotation = 90; // clockwise, about the centre
    findItem(s.doc, QStringLiteral("itm_a")).transform.scale = 0.5;
    const QImage img = render(s, 0);
    // scaled to 160x120, turned 90 deg cw: the red top-left quadrant lands top-right, green bottom-right,
    // yellow bottom-left, blue top-left; the picture now spans x 100..220, y 40..200
    SF_VERIFY_PIXEL(img, 190, 70, (Rgb{255, 0, 0}), 6);
    SF_VERIFY_PIXEL(img, 190, 170, (Rgb{0, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 130, 170, (Rgb{255, 255, 0}), 6);
    SF_VERIFY_PIXEL(img, 130, 70, (Rgb{0, 0, 255}), 6);
    SF_VERIFY_PIXEL(img, 60, 120, (Rgb{0, 0, 0}), 1);
  }

  void opacityBlendsOverTheLayerBelow() {
    const QString red = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    const QString blue = clip(QStringLiteral("blue"), solidGraph(QStringLiteral("0x0000ff"), 320, 240));
    if (red.isEmpty() || blue.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(320, 240);
    s.assets["ast_red"] = {red, AssetKind::Video};
    s.assets["ast_blue"] = {blue, AssetKind::Video};
    const QString bottom = mainTrack(s.doc);
    const QString top = addTopTrack(s.doc, QStringLiteral("trk_top"));
    addMedia(s.doc, ItemType::Video, bottom, QStringLiteral("itm_blue"), QStringLiteral("ast_blue"), 0, 40);
    Item& r = addMedia(s.doc, ItemType::Video, top, QStringLiteral("itm_red"), QStringLiteral("ast_red"), 0, 40);
    // gamma blending, as the Electron canvas does: unchanged from the reference
    blendIn(s, BlendSpace::Display);
    r.transform.opacity = 0.5;
    SF_VERIFY_PIXEL(render(s, 0), 160, 120, (Rgb{128, 0, 128}), 4);
    r.transform.opacity = 0.25;
    SF_VERIFY_PIXEL(render(s, 0), 10, 10, (Rgb{64, 0, 191}), 4);
    r.transform.opacity = 0;
    SF_VERIFY_PIXEL(render(s, 0), 10, 10, (Rgb{0, 0, 255}), 4);
    // linear light (the default): the same mix is brighter, e.g. 50% is sRGB(0.5) = 188
    blendIn(s, BlendSpace::Linear);
    r.transform.opacity = 0.5;
    SF_VERIFY_PIXEL(render(s, 0), 160, 120, (Rgb{188, 0, 188}), 4);
    r.transform.opacity = 0.25;
    SF_VERIFY_PIXEL(render(s, 0), 10, 10, (Rgb{137, 0, 225}), 4);
    r.transform.opacity = 0;
    SF_VERIFY_PIXEL(render(s, 0), 10, 10, (Rgb{0, 0, 255}), 4);
  }

  void fadeInRampsOpacity() {
    const QString red = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (red.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(red, 320, 240, 40);
    findItem(s.doc, QStringLiteral("itm_a")).props = VideoProps{{10, 0}};
    blendIn(s, BlendSpace::Display);
    SF_VERIFY_PIXEL(render(s, 0), 5, 5, (Rgb{0, 0, 0}), 2);
    SF_VERIFY_PIXEL(render(s, 5), 5, 5, (Rgb{128, 0, 0}), 4);
    SF_VERIFY_PIXEL(render(s, 12), 5, 5, (Rgb{255, 0, 0}), 4);
    blendIn(s, BlendSpace::Linear); // a fade over black is a linear-light ramp: halfway is sRGB(0.5)
    SF_VERIFY_PIXEL(render(s, 0), 5, 5, (Rgb{0, 0, 0}), 2);
    SF_VERIFY_PIXEL(render(s, 5), 5, 5, (Rgb{188, 0, 0}), 4);
    SF_VERIFY_PIXEL(render(s, 12), 5, 5, (Rgb{255, 0, 0}), 4);
  }

  void keyframedOpacity() {
    const QString red = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (red.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(red, 320, 240, 40);
    findItem(s.doc, QStringLiteral("itm_a")).keyframes[QStringLiteral("transform.opacity")] = {{0, 0.0, Easing::Linear}, {20, 1.0, Easing::Linear}};
    blendIn(s, BlendSpace::Display);
    SF_VERIFY_PIXEL(render(s, 10), 5, 5, (Rgb{128, 0, 0}), 4);
    SF_VERIFY_PIXEL(render(s, 30), 5, 5, (Rgb{255, 0, 0}), 4);
    blendIn(s, BlendSpace::Linear);
    SF_VERIFY_PIXEL(render(s, 10), 5, 5, (Rgb{188, 0, 0}), 4);
    SF_VERIFY_PIXEL(render(s, 30), 5, 5, (Rgb{255, 0, 0}), 4);
  }

  // tracks[0] is the top layer; a hidden track doesn't draw
  void trackOrderAndHidden() {
    const QString red = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    const QString blue = clip(QStringLiteral("blue"), solidGraph(QStringLiteral("0x0000ff"), 320, 240));
    if (red.isEmpty() || blue.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(320, 240);
    s.assets["ast_red"] = {red, AssetKind::Video};
    s.assets["ast_blue"] = {blue, AssetKind::Video};
    const QString bottom = mainTrack(s.doc);
    const QString top = addTopTrack(s.doc, QStringLiteral("trk_top"));
    addMedia(s.doc, ItemType::Video, bottom, QStringLiteral("itm_blue"), QStringLiteral("ast_blue"), 0, 40);
    addMedia(s.doc, ItemType::Video, top, QStringLiteral("itm_red"), QStringLiteral("ast_red"), 0, 40);
    SF_VERIFY_PIXEL(render(s, 0), 100, 100, (Rgb{255, 0, 0}), 4);
    for (Track& t : s.doc.tracks) {
      if (t.id == top) t.hidden = true;
    }
    SF_VERIFY_PIXEL(render(s, 0), 100, 100, (Rgb{0, 0, 255}), 4);
    // only the red clip's own interval shows it
    for (Track& t : s.doc.tracks) t.hidden = false;
    findItem(s.doc, QStringLiteral("itm_red")).startFrame = 10;
    findItem(s.doc, QStringLiteral("itm_red")).durationFrames = 10;
    SF_VERIFY_PIXEL(render(s, 5), 100, 100, (Rgb{0, 0, 255}), 4);
    SF_VERIFY_PIXEL(render(s, 12), 100, 100, (Rgb{255, 0, 0}), 4);
    SF_VERIFY_PIXEL(render(s, 20), 100, 100, (Rgb{0, 0, 255}), 4);
  }

  // Source frames follow sourceFrameAt: sourceIn + local * speed, in project-fps units.
  void speedAndSourceIn() {
    const QString path = dir_.filePath(QStringLiteral("index.mp4"));
    QString log;
    if (!clips_.contains(QStringLiteral("index"))) {
      if (!makeIndexClip(path, h264(), 320, 240, 25, 6, {}, &log)) QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log)));
      clips_.insert(QStringLiteral("index"), path);
    }
    Scene s = oneClip(path, 320, 240, 60);
    Item& item = findItem(s.doc, QStringLiteral("itm_a"));
    item.startFrame = 10;
    item.sourceInFrame = 20;
    item.speed = 2;
    for (const Frame f : {10, 11, 15, 30, 55}) {
      const QImage img = render(s, f);
      QCOMPARE(readIndex(img), static_cast<int>(sourceFrameAt(item, f)));
    }
    QCOMPARE(sourceFrameAt(item, 15), 30);
    // slow motion
    item.speed = 0.5;
    for (const Frame f : {10, 14, 20, 50}) QCOMPARE(readIndex(render(s, f)), static_cast<int>(sourceFrameAt(item, f)));
  }

  // A 25 fps clip in a 30 fps project: source frame n (project units) is time n/30 s.
  void sourceFramesAreProjectFpsUnits() {
    const QString path = dir_.filePath(QStringLiteral("index.mp4"));
    if (!clips_.contains(QStringLiteral("index"))) {
      QString log;
      if (!makeIndexClip(path, h264(), 320, 240, 25, 6, {}, &log)) QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log)));
      clips_.insert(QStringLiteral("index"), path);
    }
    Scene s = oneClip(path, 320, 240, 100);
    s.doc.project.fps = 30;
    for (const Frame f : {0, 30, 60, 90}) QCOMPARE(readIndex(render(s, f)), static_cast<int>(f * 25 / 30));
    QCOMPARE(readIndex(render(s, 45)), 37); // 1.5 s -> frame 37 of the 25 fps clip
  }

  void timeRemapIsHonoured() {
    const QString path = dir_.filePath(QStringLiteral("index.mp4"));
    if (!clips_.contains(QStringLiteral("index"))) {
      QString log;
      if (!makeIndexClip(path, h264(), 320, 240, 25, 6, {}, &log)) QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log)));
      clips_.insert(QStringLiteral("index"), path);
    }
    Scene s = oneClip(path, 320, 240, 40);
    Item& item = findItem(s.doc, QStringLiteral("itm_a"));
    item.timeRemap = {{0, 100}, {20, 80}}; // plays backwards
    for (const Frame f : {0, 5, 10, 19}) QCOMPARE(readIndex(render(s, f)), static_cast<int>(sourceFrameAt(item, f)));
  }

  void stillImageItem() {
    const QString png = dir_.filePath(QStringLiteral("quads.png"));
    QImage src(200, 100, QImage::Format_RGBA8888);
    src.fill(Qt::red);
    QPainter p(&src);
    p.fillRect(100, 0, 100, 50, Qt::green);
    p.fillRect(0, 50, 100, 50, Qt::blue);
    p.fillRect(100, 50, 100, 50, Qt::yellow);
    p.end();
    QVERIFY(src.save(png));
    Scene s;
    s.doc = newDoc(400, 300);
    s.assets["ast_img"] = {png, AssetKind::Image};
    addMedia(s.doc, ItemType::Image, mainTrack(s.doc), QStringLiteral("itm_i"), QStringLiteral("ast_img"), 0, 30);
    const QImage img = render(s, 0);
    // 200x100 fitted into 400x300: 400x200 centred, y 50..250
    SF_VERIFY_PIXEL(img, 100, 100, (Rgb{255, 0, 0}), 2);
    SF_VERIFY_PIXEL(img, 300, 100, (Rgb{0, 255, 0}), 2);
    SF_VERIFY_PIXEL(img, 100, 200, (Rgb{0, 0, 255}), 2);
    SF_VERIFY_PIXEL(img, 300, 200, (Rgb{255, 255, 0}), 2);
    SF_VERIFY_PIXEL(img, 100, 20, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 100, 280, (Rgb{0, 0, 0}), 1);
  }

  void stillImageWithAlpha() {
    const QString png = dir_.filePath(QStringLiteral("alpha.png"));
    QImage src(100, 100, QImage::Format_RGBA8888);
    src.fill(QColor(255, 0, 0, 128)); // 50% red
    QVERIFY(src.save(png));
    Scene s;
    s.doc = newDoc(100, 100, 25, QStringLiteral("#0000ff"));
    s.assets["ast_img"] = {png, AssetKind::Image};
    addMedia(s.doc, ItemType::Image, mainTrack(s.doc), QStringLiteral("itm_i"), QStringLiteral("ast_img"), 0, 30);
    blendIn(s, BlendSpace::Display);
    SF_VERIFY_PIXEL(render(s, 0), 50, 50, (Rgb{128, 0, 127}), 3); // straight alpha over blue
    blendIn(s, BlendSpace::Linear);
    SF_VERIFY_PIXEL(render(s, 0), 50, 50, (Rgb{188, 0, 187}), 3);
  }

  void missingMediaDrawsThePlaceholderCard() {
    Scene s;
    s.doc = newDoc(640, 360);
    s.assets["ast_gone"] = {dir_.filePath(QStringLiteral("nope.mp4")), AssetKind::Video};
    addMedia(s.doc, ItemType::Video, mainTrack(s.doc), QStringLiteral("itm_g"), QStringLiteral("ast_gone"), 0, 30);
    RenderStats stats;
    const QImage img = render(s, 0, &stats);
    QCOMPARE(stats.missing, 1);
    // the reference draws a #1f2937 card of 60 % of the canvas
    SF_VERIFY_PIXEL(img, 40, 40, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 330, 90, (Rgb{0x1f, 0x29, 0x37}), 3); // inside the card (x 128..512, y 72..288), above the caption
    SF_VERIFY_PIXEL(img, 330, 60, (Rgb{0, 0, 0}), 1);          // just above it
  }

  void shapeItem() {
    Scene s;
    s.doc = newDoc(400, 300);
    ItemInit init;
    init.id = QStringLiteral("itm_s");
    init.trackId = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    init.startFrame = 0;
    init.durationFrames = 30;
    ShapeProps sp;
    sp.shape = ShapeKind::Rect;
    sp.fill = QStringLiteral("#ff8000");
    sp.width = 100;
    sp.height = 60;
    init.props = sp;
    s.doc.items.push_back(createItem(ItemType::Shape, init));
    findItem(s.doc, QStringLiteral("itm_s")).transform.x = 50;
    const QImage img = render(s, 0);
    // centred at (250, 150): x 200..300, y 120..180
    SF_VERIFY_PIXEL(img, 250, 150, (Rgb{255, 128, 0}), 2);
    SF_VERIFY_PIXEL(img, 205, 125, (Rgb{255, 128, 0}), 2);
    SF_VERIFY_PIXEL(img, 195, 150, (Rgb{0, 0, 0}), 1);
    SF_VERIFY_PIXEL(img, 250, 190, (Rgb{0, 0, 0}), 1);
  }

  void ellipseAndTriangleShapes() {
    Scene s;
    s.doc = newDoc(200, 200);
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    for (const ShapeKind kind : {ShapeKind::Ellipse, ShapeKind::Triangle}) {
      ItemInit init;
      init.id = kind == ShapeKind::Ellipse ? QStringLiteral("itm_e") : QStringLiteral("itm_t");
      init.trackId = track;
      init.startFrame = kind == ShapeKind::Ellipse ? 0 : 10;
      init.durationFrames = 10;
      ShapeProps sp;
      sp.shape = kind;
      sp.fill = QStringLiteral("#00ff00");
      sp.width = 100;
      sp.height = 100;
      init.props = sp;
      s.doc.items.push_back(createItem(ItemType::Shape, init));
    }
    const QImage e = render(s, 0);
    SF_VERIFY_PIXEL(e, 100, 100, (Rgb{0, 255, 0}), 2);
    SF_VERIFY_PIXEL(e, 55, 55, (Rgb{0, 0, 0}), 1); // outside the circle
    const QImage t = render(s, 10);
    SF_VERIFY_PIXEL(t, 100, 130, (Rgb{0, 255, 0}), 2);
    SF_VERIFY_PIXEL(t, 60, 70, (Rgb{0, 0, 0}), 1); // upper-left corner of the box: outside the triangle
  }

  void textItemDrawsGlyphs() {
    Scene s;
    s.doc = newDoc(640, 360);
    ItemInit init;
    init.id = QStringLiteral("itm_t");
    init.trackId = addTopTrack(s.doc, QStringLiteral("trk_text"), TrackKind::Text);
    init.startFrame = 0;
    init.durationFrames = 30;
    TextProps tp;
    tp.text = QStringLiteral("HELLO");
    tp.style.fontFamily = QStringLiteral("Arial");
    tp.style.fontSize = 80;
    tp.style.color = QStringLiteral("#ffffff");
    init.props = tp;
    s.doc.items.push_back(createItem(ItemType::Text, init));
    const QImage img = render(s, 0);
    // white pixels in the middle band, none at the top
    int white = 0, top = 0;
    for (int y = 0; y < img.height(); ++y) {
      for (int x = 0; x < img.width(); ++x) {
        if (qRed(img.pixel(x, y)) > 200) (y < 100 ? top : white)++;
      }
    }
    QVERIFY2(white > 800, qPrintable(QString::number(white)));
    QCOMPARE(top, 0);
    // horizontally centred: ink spans symmetric-ish around x = 320
    int minX = img.width(), maxX = 0;
    for (int y = 0; y < img.height(); ++y) {
      for (int x = 0; x < img.width(); ++x) {
        if (qRed(img.pixel(x, y)) > 128) {
          minX = std::min(minX, x);
          maxX = std::max(maxX, x);
        }
      }
    }
    QVERIFY2(std::abs((minX + maxX) / 2 - 320) < 24, qPrintable(QStringLiteral("%1..%2").arg(minX).arg(maxX)));
  }

  void captionShowsTheActiveCard() {
    Scene s;
    s.doc = newDoc(640, 360);
    ItemInit init;
    init.id = QStringLiteral("itm_c");
    init.trackId = addTopTrack(s.doc, QStringLiteral("trk_text"), TrackKind::Text);
    init.startFrame = 0;
    init.durationFrames = 100;
    CaptionProps cp;
    cp.style.fontFamily = QStringLiteral("Arial");
    cp.style.fontSize = 48;
    cp.style.color = QStringLiteral("#ffffff");
    cp.style.highlight = CaptionHighlight::None;
    cp.maxWordsPerCard = 2;
    cp.words = {{QStringLiteral("alpha"), 0, 500, {}, {}}, {QStringLiteral("beta"), 500, 1000, {}, {}}, {QStringLiteral("gamma"), 1000, 1500, {}, {}}};
    init.props = cp;
    s.doc.items.push_back(createItem(ItemType::Caption, init));
    const auto ink = [](const QImage& img) {
      int n = 0;
      for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) n += qRed(img.pixel(x, y)) > 200;
      }
      return n;
    };
    const QImage first = render(s, 0);  // card "alpha beta"
    const QImage third = render(s, 30); // 1.2 s: card "gamma"
    QVERIFY(ink(first) > 500);
    QVERIFY(ink(third) > 200);
    QVERIFY(ink(first) > ink(third)); // two words vs one
    // placed at placementY = 0.82 of the height
    int sumY = 0, n = 0;
    for (int y = 0; y < first.height(); ++y) {
      for (int x = 0; x < first.width(); ++x) {
        if (qRed(first.pixel(x, y)) > 128) {
          sumY += y;
          ++n;
        }
      }
    }
    QVERIFY(n > 0);
    QVERIFY2(std::abs(sumY / n - 0.82 * 360) < 20, qPrintable(QString::number(sumY / n)));
    // before the first word nothing shows
    cp.words[0].startMs = 200;
    findItem(s.doc, QStringLiteral("itm_c")).props = cp;
    QCOMPARE(ink(render(s, 0)), 0);
  }

  void colourEffectsApply() {
    const QString red = clip(QStringLiteral("red"), solidGraph(QStringLiteral("0xff0000"), 320, 240));
    if (red.isEmpty()) QSKIP(qPrintable(skip_));
    Scene s = oneClip(red, 320, 240, 40);
    Effect e;
    e.id = QStringLiteral("fx_1");
    e.type = QStringLiteral("grayscale");
    e.params[QStringLiteral("amount")] = 1.0;
    findItem(s.doc, QStringLiteral("itm_a")).effects.push_back(e);
    // 0.2126 * 255 in every channel
    SF_VERIFY_PIXEL(render(s, 0), 100, 100, (Rgb{54, 54, 54}), 4);
    findItem(s.doc, QStringLiteral("itm_a")).effects.clear();
    e.type = QStringLiteral("brightness");
    e.params.clear();
    e.params[QStringLiteral("amount")] = -0.5;
    findItem(s.doc, QStringLiteral("itm_a")).effects.push_back(e);
    SF_VERIFY_PIXEL(render(s, 0), 100, 100, (Rgb{128, 0, 0}), 4);
  }

  // Phone clips carry a display matrix; the compositor must show them upright.
  void rotationMetadataIsApplied() {
    const QString plain = clip(QStringLiteral("quads_land"), quadrantsGraph(320, 240));
    if (plain.isEmpty()) QSKIP(qPrintable(skip_));
    const QString rotated = dir_.filePath(QStringLiteral("rot.mp4"));
    if (!runFfmpeg({"-display_rotation:v:0", "90", "-i", plain, "-c", "copy", rotated})) QSKIP("cannot write a display matrix");
    const auto info = probeMedia(rotated);
    QVERIFY(info);
    if (info->rotation == 0) QSKIP("muxer did not keep the display matrix");
    QCOMPARE(info->displayWidth, 240);
    Scene s = oneClip(rotated, 240, 320);
    const QImage img = render(s, 0);
    QCOMPARE(img.size(), QSize(240, 320));
    // rotation r = degrees clockwise to turn the stored frame upright
    if (info->rotation == 90) {
      // red (stored top-left) ends up top-right, green bottom-right, yellow bottom-left, blue top-left
      SF_VERIFY_PIXEL(img, 180, 80, (Rgb{255, 0, 0}), 8);
      SF_VERIFY_PIXEL(img, 180, 240, (Rgb{0, 255, 0}), 8);
      SF_VERIFY_PIXEL(img, 60, 240, (Rgb{255, 255, 0}), 8);
      SF_VERIFY_PIXEL(img, 60, 80, (Rgb{0, 0, 255}), 8);
    } else {
      QCOMPARE(info->rotation, 270); // stored top-left ends up bottom-left
      SF_VERIFY_PIXEL(img, 60, 240, (Rgb{255, 0, 0}), 8);
      SF_VERIFY_PIXEL(img, 60, 80, (Rgb{0, 255, 0}), 8);
      SF_VERIFY_PIXEL(img, 180, 80, (Rgb{255, 255, 0}), 8);
      SF_VERIFY_PIXEL(img, 180, 240, (Rgb{0, 0, 255}), 8);
    }
  }

  // ---- zero-copy path ----

  void gpuPathIsUsedWhenAvailable() {
    if (!gpu_->compositor().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(640, 360));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 640, 360);
    RenderStats stats;
    QString decoder;
    const QImage img = renderWith(s, 5, true, HwMode::Auto, &stats, &decoder);
    if (!decoder.contains(QLatin1String("d3d11va"))) QSKIP(qPrintable(QStringLiteral("no D3D11VA here: %1").arg(decoder)));
    QCOMPARE(stats.gpuLayers, 1);
    QCOMPARE(stats.cpuLayers, 0);
    SF_VERIFY_PIXEL(img, 100, 50, (Rgb{255, 0, 0}), 6);
    SF_VERIFY_PIXEL(img, 500, 300, (Rgb{255, 255, 0}), 6);
  }

  // Same hardware frames, sampled in place vs downloaded and converted by swscale: the shader's YUV->RGB
  // must agree with frame_convert for every matrix / range the stream can be tagged with.
  void gpuConversionMatchesTheCpuConversion_data() {
    QTest::addColumn<QString>("matrix");
    QTest::addColumn<QString>("range");
    QTest::newRow("bt709 limited") << "bt709" << "tv";
    QTest::newRow("bt601 limited") << "smpte170m" << "tv";
    QTest::newRow("bt709 full") << "bt709" << "pc";
    QTest::newRow("bt2020 limited") << "bt2020nc" << "tv";
    QTest::newRow("untagged") << "" << "";
  }
  void gpuConversionMatchesTheCpuConversion() {
    QFETCH(QString, matrix);
    QFETCH(QString, range);
    if (!gpu_->compositor().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    QStringList codec{"-c:v", "h264_nvenc", "-g", "30", "-b:v", "8M"};
    QString vf = QStringLiteral("smptebars=s=640x360:r=25:d=2,format=yuv420p");
    if (!matrix.isEmpty()) {
      vf = QStringLiteral("smptebars=s=640x360:r=25:d=2,scale=out_color_matrix=%1:out_range=%2,format=yuv420p").arg(
          matrix == QLatin1String("smpte170m") ? QStringLiteral("bt601") : matrix, range);
      codec += {"-colorspace", matrix, "-color_range", range, "-color_primaries", "bt709", "-color_trc", "bt709"};
    }
    const QString name = QStringLiteral("bars_%1_%2").arg(matrix, range);
    const QString path = clip(name, vf, codec);
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const auto info = probeMedia(path);
    QVERIFY(info);
    qInfo() << name << "tags:" << info->color.matrix << info->color.range;
    const Scene s = oneClip(path, 640, 360);
    RenderStats stats;
    QString decoder;
    const QImage viaGpu = renderWith(s, 4, true, HwMode::Required, &stats, &decoder);
    if (stats.gpuLayers != 1) QSKIP(qPrintable(QStringLiteral("zero-copy not available: %1").arg(decoder)));
    // same frames, forced through the download path
    gpu_->compositor().setGpuEnabled(false);
    FrameService::Options o;
    o.hw = HwMode::Required;
    o.gpuFrames = true;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    const QImage viaCpu = gpu_->render(s.doc, 4, prov, &stats);
    QCOMPARE(stats.cpuLayers, 1);
    gpu_->compositor().setGpuEnabled(true);
    const double mean = meanAbsDiff(viaGpu, viaCpu);
    const int worst = maxAbsDiff(viaGpu, viaCpu);
    qInfo() << name << "mean abs diff" << mean << "max" << worst;
    QVERIFY2(mean < 1.5, qPrintable(QString::number(mean)));
    // chroma edges differ a little (sws vs bilinear chroma siting); flat areas must be tight
    QVERIFY2(worst < 90, qPrintable(QString::number(worst)));
    for (const QPoint p : {QPoint(40, 100), QPoint(200, 100), QPoint(320, 100), QPoint(560, 100), QPoint(50, 300), QPoint(320, 330)}) {
      QVERIFY2(closeTo(viaGpu.pixel(p), {qRed(viaCpu.pixel(p)), qGreen(viaCpu.pixel(p)), qBlue(viaCpu.pixel(p))}, 3),
               qPrintable(QStringLiteral("%1 gpu %2 cpu %3").arg(name, describe(viaGpu.pixel(p)), describe(viaCpu.pixel(p)))));
    }
  }

  void tenBitHevcThroughP010() {
    if (!gpu_->compositor().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    const QString path = clip(QStringLiteral("bars10"), QStringLiteral("smptebars=s=640x360:r=25:d=2,format=p010le"),
                              {"-c:v", "hevc_nvenc", "-profile:v", "main10", "-g", "30", "-b:v", "8M"});
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 640, 360);
    RenderStats stats;
    QString decoder;
    const QImage viaGpu = renderWith(s, 4, true, HwMode::Required, &stats, &decoder);
    if (stats.gpuLayers != 1) QSKIP(qPrintable(QStringLiteral("zero-copy not available: %1").arg(decoder)));
    const QImage viaSw = renderWith(s, 4, false, HwMode::Off, &stats);
    const double mean = meanAbsDiff(viaGpu, viaSw);
    qInfo() << "10-bit hevc: GPU (P010 shader) vs software decode mean abs diff" << mean;
    QVERIFY2(mean < 2.5, qPrintable(QString::number(mean)));
  }

  void hardwareAndSoftwareDecodersAgree() {
    if (!gpu_->compositor().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    const QString path = clip(QStringLiteral("quads"), quadrantsGraph(640, 360));
    if (path.isEmpty()) QSKIP(qPrintable(skip_));
    const Scene s = oneClip(path, 640, 360);
    RenderStats stats;
    QString decoder;
    const QImage hw = renderWith(s, 7, true, HwMode::Auto, &stats, &decoder);
    if (stats.gpuLayers != 1) QSKIP(qPrintable(QStringLiteral("zero-copy not available: %1").arg(decoder)));
    const QImage sw = renderWith(s, 7, false, HwMode::Off);
    const double mean = meanAbsDiff(hw, sw);
    qInfo() << "hardware decode + shader vs software decode + swscale: mean abs diff" << mean;
    QVERIFY2(mean < 1.5, qPrintable(QString::number(mean)));
  }

  void gpuFrameCountStaysWithinTheBudget() {
    if (!gpu_->compositor().gpuCapable()) QSKIP("QRhi is not on the shared D3D11 device");
    const QString path = dir_.filePath(QStringLiteral("long.mp4"));
    if (!clips_.contains(QStringLiteral("long"))) {
      QString log;
      if (!makeIndexClip(path, h264(), 640, 360, 25, 6, {}, &log)) QSKIP(qPrintable(QStringLiteral("cannot encode: %1").arg(log)));
      clips_.insert(QStringLiteral("long"), path);
    }
    FrameService::Options o;
    o.hw = HwMode::Auto;
    o.gpuFrames = true;
    o.gpuFramesPerAsset = 4;
    o.cacheBytes = 256ll << 20;
    FrameService svc(o);
    const AssetId id = svc.openAsset(path);
    VideoFramePtr first = svc.frameBlocking(id, 0);
    QVERIFY(first);
    if (!first->gpu) QSKIP("not decoding through D3D11VA here");
    // decode everything: far more frames than the pool holds, none of them may starve the decoder
    for (qint64 n = 0; n < 150; ++n) {
      const VideoFramePtr f = svc.frameBlocking(id, n);
      QVERIFY2(f, qPrintable(QStringLiteral("frame %1 failed").arg(n)));
      QVERIFY(svc.cache().gpuCount() <= 4);
    }
    // paused/scrubbing: frames that fall out of the GPU budget are demoted to RGBA, not lost
    svc.setPlayhead(id, 0, 0);
    svc.frameBlocking(id, 10);
    for (qint64 n = 11; n < 30; ++n) svc.frameBlocking(id, n);
    QVERIFY(svc.cache().gpuCount() <= 4);
    int demoted = 0;
    for (qint64 n = 10; n < 30; ++n) {
      if (const auto f = svc.cache().get(id, n); f && !f->gpu && !f->image.isNull()) ++demoted;
    }
    qInfo() << "demoted frames in cache:" << demoted << "gpu frames:" << svc.cache().gpuCount();
    QVERIFY(demoted > 0);
  }
};

QTEST_MAIN(TstCompositor)
#include "tst_compositor.moc"
