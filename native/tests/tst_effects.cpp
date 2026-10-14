// GPU effects: the schema (round trip, old projects), the param schema and its mapping into the uniform block,
// keyframe interpolation, and rendered pictures: vignette, blur (both passes), chromatic aberration, crossfade and
// wipe through the generated shaders, and the no-effect path. Frames are tiny (128x72); stills and shapes only, so no
// ffmpeg is needed. If no D3D11 device can be created the GPU tests skip.

#include "core/apply.h"
#include "render/effects.h"
#include "render/offscreen.h"
#include "render_testutil.h"

#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <set>

using namespace sf;
using namespace sf::render;
using namespace sf::test;

namespace {

constexpr int kW = 128, kH = 72;

QString Q(const char* s) { return QString::fromLatin1(s); }

FxInstance fx(const QString& id, const QString& effectId, const QJsonObject& params = {}) {
  FxInstance f;
  f.id = id;
  f.effectId = effectId;
  f.params = params;
  return f;
}

} // namespace

class TstEffects : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  std::unique_ptr<OffscreenRenderer> gpu_;
  QString skip_;
  QString halves_, checker_, pattern_;

  struct Scene {
    TimelineDoc doc;
    AssetTable assets;
  };

  QImage render(const Scene& s, Frame frame, RenderStats* stats = nullptr) {
    FrameService::Options o;
    o.hw = HwMode::Off;
    o.gpuFrames = false;
    FrameService svc(o);
    FrameServiceProvider prov(svc, s.assets, FrameServiceProvider::Mode::Blocking);
    gpu_->compositor().setGpuEnabled(false);
    return gpu_->render(s.doc, frame, prov, stats);
  }

  static void blendIn(Scene& s, BlendSpace space) {
    ColorManagement cm = s.doc.project.colorManagement.value_or(ColorManagement{});
    cm.blendSpace = space;
    s.doc.project.colorManagement = cm;
  }

  // A still filling the canvas
  Scene still(const QString& png) {
    Scene s;
    s.doc = newDoc(kW, kH);
    s.assets["ast_img"] = {png, AssetKind::Image};
    addMedia(s.doc, ItemType::Image, mainTrack(s.doc), QStringLiteral("itm_i"), QStringLiteral("ast_img"), 0, 40);
    return s;
  }

  // A solid rectangle filling the canvas, on its own overlay track (shapes can't sit on the video track)
  static Item& solid(Scene& s, const QString& id, const QString& track, const QString& colour, Frame start, Frame duration) {
    ItemInit init;
    init.id = id;
    init.trackId = track;
    init.startFrame = start;
    init.durationFrames = duration;
    ShapeProps sp;
    sp.shape = ShapeKind::Rect;
    sp.fill = colour;
    sp.width = kW;
    sp.height = kH;
    init.props = sp;
    s.doc.items.push_back(createItem(ItemType::Shape, init));
    return s.doc.items.back();
  }

  // Red 0..20 then blue 20..40 on one track, butted
  static Scene redThenBlue() {
    Scene s;
    s.doc = newDoc(kW, kH);
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    solid(s, QStringLiteral("itm_red"), track, QStringLiteral("#ff0000"), 0, 20);
    solid(s, QStringLiteral("itm_blue"), track, QStringLiteral("#0000ff"), 20, 20);
    blendIn(s, BlendSpace::Display); // values mix as written: 50% of red and blue is (128, 0, 128)
    return s;
  }

  static FxTransition transition(const QString& effectId, Frame duration, const QJsonObject& params = {}, const QString& align = QStringLiteral("center")) {
    FxTransition t;
    t.id = QStringLiteral("trn_0123456789");
    t.effectId = effectId;
    t.fromItemId = QStringLiteral("itm_red");
    t.toItemId = QStringLiteral("itm_blue");
    t.duration = duration;
    t.align = align;
    t.params = params;
    return t;
  }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    QString err;
    gpu_ = OffscreenRenderer::create(&err);
    if (!gpu_) skip_ = QStringLiteral("no D3D11 QRhi: %1").arg(err);
    // left half white, right half black
    halves_ = dir_.filePath(QStringLiteral("halves.png"));
    QImage h(kW, kH, QImage::Format_RGBA8888);
    h.fill(Qt::black);
    QPainter(&h).fillRect(0, 0, kW / 2, kH, Qt::white);
    QVERIFY(h.save(halves_));
    // top-left and bottom-right white, the rest black
    checker_ = dir_.filePath(QStringLiteral("checker.png"));
    QImage c(kW, kH, QImage::Format_RGBA8888);
    c.fill(Qt::black);
    {
      QPainter p(&c);
      p.fillRect(0, 0, kW / 2, kH / 2, Qt::white);
      p.fillRect(kW / 2, kH / 2, kW / 2, kH / 2, Qt::white);
    }
    QVERIFY(c.save(checker_));
    // test pattern: a horizontal gradient on the top half, an 8 px checker below
    pattern_ = dir_.filePath(QStringLiteral("pattern.png"));
    QImage pat(kW, kH, QImage::Format_RGBA8888);
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x) {
        const bool top = y < kH / 2;
        const int v = top ? x * 255 / (kW - 1) : (((x / 8) + (y / 8)) % 2 ? 255 : 0);
        pat.setPixel(x, y, qRgb(v, top ? v : v / 2 + 20, top ? 255 - v : v));
      }
    QVERIFY(pat.save(pattern_));
  }

  // ---- schema ----
  void registryHasTheStarters() {
    const FxRegistry& reg = FxRegistry::instance();
    QVERIFY2(reg.error().isEmpty(), qPrintable(reg.error()));
    for (const char* id : {"vignette", "gaussian_blur", "chromatic_aberration"}) {
      const FxDef* d = reg.find(Q(id));
      QVERIFY2(d, id);
      QCOMPARE(d->kind, FxKind::Filter);
      QVERIFY(!d->whenToUse.isEmpty());
    }
    for (const char* id : {"crossfade", "wipe"}) {
      const FxDef* d = reg.find(Q(id));
      QVERIFY2(d, id);
      QCOMPARE(d->kind, FxKind::Transition);
    }
    QCOMPARE(reg.find(Q("gaussian_blur"))->passes, 2);
    const QJsonObject j = fxDefJson(*reg.find(Q("wipe")));
    QCOMPARE(j.value(Q("kind")).toString(), Q("transition"));
    QCOMPARE(j.value(Q("params")).toArray().size(), 2);
    QCOMPARE(j.value(Q("params")).toArray().at(0).toObject().value(Q("type")).toString(), Q("vec2"));
  }

  void projectRoundTripAndOldProjects() {
    TimelineDoc doc = newDoc(kW, kH);
    Scene s;
    s.doc = doc;
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    solid(s, QStringLiteral("itm_red"), track, QStringLiteral("#ff0000"), 0, 20);
    solid(s, QStringLiteral("itm_blue"), track, QStringLiteral("#0000ff"), 20, 20);
    FxInstance f = fx(QStringLiteral("fx_0123456789"), QStringLiteral("chromatic_aberration"), QJsonObject{{Q("amount"), 0.01}, {Q("radial"), true}});
    f.enabled = false;
    f.keyframes[QStringLiteral("amount")] = {Keyframe{0, 0.0, Easing::Linear}, Keyframe{10, 0.02, Easing::EaseInOut}};
    s.doc.items[0].fx = {f, fx(QStringLiteral("fx_abcdef0123"), QStringLiteral("vignette"))};
    s.doc.transitions = {transition(QStringLiteral("wipe"), 12, QJsonObject{{Q("direction"), QJsonArray{0, 1}}}, QStringLiteral("end"))};

    const QByteArray json = timelineDocToJson(s.doc);
    const TimelineDoc back = parseTimelineDocJson(json);
    QVERIFY(back == s.doc);
    QCOMPARE(back.items[0].fx.size(), size_t(2));
    QVERIFY(!back.items[0].fx[0].enabled);
    QCOMPARE(back.items[0].fx[0].keyframes.at(QStringLiteral("amount")).size(), size_t(2));
    QCOMPARE(back.transitions.at(0).align, QStringLiteral("end"));

    // a project written before effects existed has neither key, loads with no effects, and writes neither back
    TimelineDoc plain = s.doc;
    plain.transitions.clear();
    for (Item& it : plain.items) it.fx.clear();
    const QByteArray old = timelineDocToJson(plain);
    QVERIFY(!old.contains("\"fx\""));
    QVERIFY(!old.contains("transitions"));
    const TimelineDoc loaded = parseTimelineDocJson(old);
    QVERIFY(loaded.transitions.empty());
    QVERIFY(loaded.items[0].fx.empty());
    QVERIFY(loaded == plain);

    // bad align is rejected like any schema violation
    QJsonObject bad = toJson(s.doc.transitions.at(0));
    bad.insert(Q("align"), Q("sideways"));
    QVERIFY_THROWS_EXCEPTION(SchemaError, parseFxTransition(Rd(bad)));

    // as ops: one undoable patch of the stack, one of the transition list
    ItemPatch patch;
    patch.fx = std::vector<FxInstance>{};
    const ApplyResult cleared = applyOp(s.doc, ItemUpdate{QStringLiteral("itm_red"), patch});
    QVERIFY(cleared.doc.items[0].fx.empty());
    const ApplyResult undone = applyOps(cleared.doc, cleared.inverse);
    QVERIFY(undone.doc == s.doc);
    const ApplyResult noTr = applyOp(s.doc, TransitionsSet{});
    QVERIFY(noTr.doc.transitions.empty());
    QVERIFY(applyOps(noTr.doc, noTr.inverse).doc == s.doc);
    const Op viaJson = parseOp(Rd(toJson(Op(TransitionsSet{s.doc.transitions}))));
    QVERIFY(viaJson == Op(TransitionsSet{s.doc.transitions}));
  }

  void windowsFollowTheAlignment() {
    Scene s = redThenBlue();
    const Item *a = nullptr, *b = nullptr;
    Frame start = 0, end = 0;
    s.doc.transitions = {transition(QStringLiteral("crossfade"), 10)};
    QVERIFY(transitionWindow(s.doc, s.doc.transitions[0], &a, &b, &start, &end));
    QCOMPARE(start, Frame(15));
    QCOMPARE(end, Frame(25));
    s.doc.transitions[0].align = QStringLiteral("start");
    QVERIFY(transitionWindow(s.doc, s.doc.transitions[0], &a, &b, &start, &end));
    QCOMPARE(start, Frame(20));
    s.doc.transitions[0].align = QStringLiteral("end");
    QVERIFY(transitionWindow(s.doc, s.doc.transitions[0], &a, &b, &start, &end));
    QCOMPARE(end, Frame(20));
    // no longer butted: dormant
    s.doc.items[1].startFrame = 22;
    QVERIFY(!transitionWindow(s.doc, s.doc.transitions[0], &a, &b, &start, &end));
    QVERIFY(activeTransitions(s.doc, 20).empty());
  }

  // ---- params ----
  void paramsAreCheckedAndClamped() {
    const FxDef& vig = *FxRegistry::instance().find(Q("vignette"));
    QJsonObject out;
    QString why;
    QVERIFY(normalizeFxParams(vig, QJsonObject{{Q("amount"), 3.0}}, &out, &why));
    QCOMPARE(out.value(Q("amount")).toDouble(), 1.0);
    QVERIFY(!normalizeFxParams(vig, QJsonObject{{Q("nope"), 1}}, &out, &why));
    QVERIFY(why.contains(Q("amount"))); // names what exists
    QVERIFY(!normalizeFxParams(vig, QJsonObject{{Q("amount"), Q("lots")}}, &out, &why));
    const FxDef& wipe = *FxRegistry::instance().find(Q("wipe"));
    QVERIFY(!normalizeFxParams(wipe, QJsonObject{{Q("direction"), 1}}, &out, &why));
    QVERIFY(normalizeFxParams(wipe, QJsonObject{{Q("direction"), QJsonArray{0, 5}}}, &out, &why));
    QCOMPARE(out.value(Q("direction")).toArray().at(1).toDouble(), 1.0);
  }

  void paramsMapIntoTheUniformBlock() {
    QCOMPARE(sizeof(FxUniforms), size_t(352));
    // every param type, in schema order: slot i is vec4 i
    QByteArray text = "{'effects':[{'id':'probe','kind':'filter','params':["
                      "{'name':'gain','type':'float','min':0,'max':4,'default':2},"
                      "{'name':'size','type':'vec2','min':-2,'max':2,'default':[0.5,-1]},"
                      "{'name':'tint','type':'color','default':'#ff8000'},"
                      "{'name':'flag','type':'bool','default':true},"
                      "{'name':'count','type':'int','min':0,'max':9,'default':3},"
                      "{'name':'mode','type':'enum','options':['a','b','c'],'default':'c'},"
                      "{'name':'spot','type':'point','default':[0.25,0.25]}]}]}";
    const QJsonObject doc = QJsonDocument::fromJson(text.replace('\'', '"')).object();
    std::vector<FxDef> defs;
    QString err;
    QVERIFY2(parseFxDefs(doc, &defs, &err), qPrintable(err));
    const FxDef& d = defs.at(0);
    float vec[kFxMaxParams][4];

    packFxParams(d, resolveFxParams(d, {}, {}, 0), true, vec); // defaults
    QCOMPARE(vec[0][0], 2.0f);
    QCOMPARE(vec[1][0], 0.5f);
    QCOMPARE(vec[1][1], -1.0f);
    QVERIFY(qAbs(vec[2][0] - 1.0f) < 1e-5f);                   // #ff8000 in linear light
    QVERIFY(qAbs(vec[2][1] - 0.2158605f) < 1e-3f);
    QCOMPARE(vec[2][2], 0.0f);
    QCOMPARE(vec[2][3], 1.0f);
    QCOMPARE(vec[3][0], 1.0f);
    QCOMPARE(vec[4][0], 3.0f);
    QCOMPARE(vec[5][0], 2.0f);                                  // enum index
    QCOMPARE(vec[6][0], 0.25f);
    QCOMPARE(vec[6][1], 0.75f);                                 // point: top-left origin in, y-up out
    QCOMPARE(vec[7][0], 0.0f);                                  // unused vec are zero

    packFxParams(d, resolveFxParams(d, QJsonObject{{Q("tint"), QJsonArray{0.5, 0.5, 0.5, 0.25}}, {Q("mode"), Q("b")}, {Q("flag"), false}}, {}, 0), false, vec);
    QCOMPARE(vec[2][0], 0.5f); // display-space blending: colours are not linearised
    QCOMPARE(vec[2][3], 0.25f);
    QCOMPARE(vec[5][0], 1.0f);
    QCOMPARE(vec[3][0], 0.0f);

    // the shipped wipe: direction in slot 0 (.xy), softness in slot 1
    const FxDef& wipe = *FxRegistry::instance().find(Q("wipe"));
    packFxParams(wipe, resolveFxParams(wipe, QJsonObject{{Q("direction"), QJsonArray{0, 1}}, {Q("softness"), 0.2}}, {}, 0), true, vec);
    QCOMPARE(vec[0][0], 0.0f);
    QCOMPARE(vec[0][1], 1.0f);
    QCOMPARE(vec[1][0], 0.2f);
  }

  void keyframesInterpolate() {
    const FxDef& vig = *FxRegistry::instance().find(Q("vignette"));
    KeyframeMap kf;
    kf[QStringLiteral("amount")] = {Keyframe{0, 0.0, Easing::Linear}, Keyframe{10, 1.0, Easing::Linear}, Keyframe{20, 0.0, Easing::Hold}};
    const auto at = [&](Frame f) { return resolveFxParams(vig, QJsonObject{{Q("amount"), 0.9}}, kf, f).value(Q("amount")).toDouble(); };
    QCOMPARE(at(-5), 0.0); // before the first: its value
    QCOMPARE(at(0), 0.0);
    QVERIFY(qAbs(at(5) - 0.5) < 1e-9);
    QCOMPARE(at(10), 1.0);
    QCOMPARE(at(15), 1.0);  // hold keeps the previous value until the next key
    QCOMPARE(at(20), 0.0);
    QCOMPARE(at(99), 0.0);
    // unanimated params keep their static value, and keyframes don't touch other params
    QCOMPARE(resolveFxParams(vig, QJsonObject{{Q("radius"), 0.3}}, kf, 5).value(Q("radius")).toDouble(), 0.3);
    // components of a vector animate on their own
    const FxDef& wipe = *FxRegistry::instance().find(Q("wipe"));
    KeyframeMap comp;
    comp[QStringLiteral("direction.y")] = {Keyframe{0, 0.0, Easing::Linear}, Keyframe{10, 1.0, Easing::Linear}};
    const QJsonArray dir = resolveFxParams(wipe, {}, comp, 5).value(Q("direction")).toArray();
    QCOMPARE(dir.at(0).toDouble(), 1.0); // from the default [1, 0]
    QVERIFY(qAbs(dir.at(1).toDouble() - 0.5) < 1e-9);
    QString why;
    QVERIFY(fxKeyframeKeyOk(wipe, QStringLiteral("direction.y"), &why));
    QVERIFY(!fxKeyframeKeyOk(wipe, QStringLiteral("direction"), &why)); // a vec2 as a whole can't be keyed
    QVERIFY(!fxKeyframeKeyOk(wipe, QStringLiteral("direction.r"), &why));
    QVERIFY(!fxKeyframeKeyOk(wipe, QStringLiteral("nope"), &why));
    QVERIFY(fxKeyframeKeyOk(vig, QStringLiteral("amount"), &why));
  }

  // ---- pictures ----
  void vignetteDarkensTheCornersNotTheCentre() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(kW, kH);
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    solid(s, QStringLiteral("itm_s"), track, QStringLiteral("#ffffff"), 0, 30);
    const QImage plain = render(s, 0);
    SF_VERIFY_PIXEL(plain, 2, 2, (Rgb{255, 255, 255}), 1);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_v"), QStringLiteral("vignette"), QJsonObject{{Q("amount"), 1.0}, {Q("radius"), 0.3}, {Q("softness"), 0.5}})};
    RenderStats stats;
    const QImage img = render(s, 0, &stats);
    QCOMPARE(stats.fxPasses, 1);
    QCOMPARE(stats.fxErrors, 0);
    SF_VERIFY_PIXEL(img, kW / 2, kH / 2, (Rgb{255, 255, 255}), 3);
    const QRgb corner = img.pixel(1, 1);
    QVERIFY2(qRed(corner) < 40 && qGreen(corner) < 40 && qBlue(corner) < 40, qPrintable(describe(corner)));
    const QRgb mid = img.pixel(kW / 4, kH / 4); // part way out: between the two
    QVERIFY2(qRed(mid) > qRed(corner) && qRed(mid) < 255, qPrintable(describe(mid)));
  }

  void effectParamsAnimateWithTheClip() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s;
    s.doc = newDoc(kW, kH);
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    solid(s, QStringLiteral("itm_s"), track, QStringLiteral("#ffffff"), 5, 30); // starts at frame 5: keyframes are item-local
    FxInstance v = fx(QStringLiteral("fx_v"), QStringLiteral("vignette"), QJsonObject{{Q("radius"), 0.3}});
    v.keyframes[QStringLiteral("amount")] = {Keyframe{0, 0.0, Easing::Linear}, Keyframe{10, 1.0, Easing::Linear}};
    s.doc.items[0].fx = {v};
    SF_VERIFY_PIXEL(render(s, 5), 1, 1, (Rgb{255, 255, 255}), 2);
    const QRgb dark = render(s, 15).pixel(1, 1);
    QVERIFY2(qRed(dark) < 40, qPrintable(describe(dark)));
    const QRgb half = render(s, 10).pixel(1, 1);
    QVERIFY2(qRed(half) > qRed(dark) + 20 && qRed(half) < 250, qPrintable(describe(half)));
  }

  void noEffectPathIsUntouched() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(halves_);
    RenderStats base;
    const QImage plain = render(s, 3, &base);
    QCOMPARE(base.fxPasses, 0);
    SF_VERIFY_PIXEL(plain, 10, 10, (Rgb{255, 255, 255}), 1);
    SF_VERIFY_PIXEL(plain, 120, 10, (Rgb{0, 0, 0}), 1);
    // a stack of bypassed effects and a transition that isn't running draw exactly the same pixels, with no passes
    s.doc.items[0].fx = {fx(QStringLiteral("fx_a"), QStringLiteral("gaussian_blur")), fx(QStringLiteral("fx_b"), QStringLiteral("vignette"))};
    s.doc.items[0].fx[0].enabled = false;
    s.doc.items[0].fx[1].enabled = false;
    s.doc.transitions = {transition(QStringLiteral("crossfade"), 4)}; // refers to clips this doc doesn't have
    RenderStats stats;
    const QImage again = render(s, 3, &stats);
    QCOMPARE(stats.fxPasses, 0);
    QCOMPARE(stats.layers, 1);
    QCOMPARE(maxAbsDiff(plain, again), 0);
    // unknown effects are skipped and counted, the picture stays
    s.doc.items[0].fx = {fx(QStringLiteral("fx_x"), QStringLiteral("no_such_effect"))};
    const QImage skipped = render(s, 3, &stats);
    QCOMPARE(stats.fxErrors, 1);
    QCOMPARE(maxAbsDiff(plain, skipped), 0);
    // a filter put through the pipeline with nothing to do gives the same picture back
    s.doc.items[0].fx = {fx(QStringLiteral("fx_n"), QStringLiteral("gaussian_blur"), QJsonObject{{Q("radius"), 0.0}})};
    const QImage through = render(s, 3, &stats);
    QCOMPARE(stats.fxPasses, 2);
    QVERIFY2(maxAbsDiff(plain, through) <= 2, qPrintable(QString::number(maxAbsDiff(plain, through))));
  }

  void blurSoftensEdgesInBothDirections() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(checker_);
    const QImage sharp = render(s, 0);
    SF_VERIFY_PIXEL(sharp, 30, kH / 2 - 2, (Rgb{255, 255, 255}), 1); // top-left white
    SF_VERIFY_PIXEL(sharp, 30, kH / 2 + 2, (Rgb{0, 0, 0}), 1);       // bottom-left black
    s.doc.items[0].fx = {fx(QStringLiteral("fx_b"), QStringLiteral("gaussian_blur"), QJsonObject{{Q("radius"), 10.0}})};
    RenderStats stats;
    const QImage img = render(s, 0, &stats);
    QCOMPARE(stats.fxPasses, 2);
    // across the horizontal edge (only the vertical pass can soften it) and the vertical one (only the horizontal pass)
    const QRgb up = img.pixel(30, kH / 2 - 2), down = img.pixel(30, kH / 2 + 2);
    QVERIFY2(qRed(up) > 90 && qRed(up) < 230 && qRed(down) > 90 && qRed(down) < 230, qPrintable(describe(up) + describe(down)));
    const QRgb left = img.pixel(kW / 2 - 2, 15), right = img.pixel(kW / 2 + 2, 15);
    QVERIFY2(qRed(left) > 90 && qRed(left) < 230 && qRed(right) > 90 && qRed(right) < 230, qPrintable(describe(left) + describe(right)));
    SF_VERIFY_PIXEL(img, 8, 8, (Rgb{255, 255, 255}), 6); // far from any edge nothing changes
  }

  void chromaticAberrationSplitsTheChannels() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(halves_);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_c"), QStringLiteral("chromatic_aberration"), QJsonObject{{Q("amount"), 0.05}, {Q("angle"), 0.0}})};
    const QImage img = render(s, 0);
    // 6.4 px split: just left of the white/black edge red reads the black side, blue stays on white
    const QRgb px = img.pixel(kW / 2 - 3, kH / 2);
    QVERIFY2(qRed(px) < 40 && qGreen(px) > 215 && qBlue(px) > 215, qPrintable(describe(px)));
    const QRgb right = img.pixel(kW / 2 + 3, kH / 2); // on the black side blue reaches back to the white
    QVERIFY2(qRed(right) < 40 && qGreen(right) < 40 && qBlue(right) > 215, qPrintable(describe(right)));
    SF_VERIFY_PIXEL(img, 8, 8, (Rgb{255, 255, 255}), 2);
  }

  void crossfadeAtHalfIsTheMean() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = redThenBlue();
    s.doc.transitions = {transition(QStringLiteral("crossfade"), 10)}; // frames 15..24, progress (f - 15) / 10
    RenderStats stats;
    const QImage mid = render(s, 20, &stats);
    QCOMPARE(stats.fxPasses, 1);
    QCOMPARE(stats.layers, 2);
    SF_VERIFY_PIXEL(mid, kW / 2, kH / 2, (Rgb{128, 0, 128}), 3);
    SF_VERIFY_PIXEL(mid, 5, 5, (Rgb{128, 0, 128}), 3);
    SF_VERIFY_PIXEL(render(s, 15), kW / 2, kH / 2, (Rgb{255, 0, 0}), 2); // window start: all outgoing
    SF_VERIFY_PIXEL(render(s, 14), kW / 2, kH / 2, (Rgb{255, 0, 0}), 1); // outside: plain cut behaviour
    SF_VERIFY_PIXEL(render(s, 25), kW / 2, kH / 2, (Rgb{0, 0, 255}), 1);
    const QImage quarter = render(s, 17); // progress 0.2
    SF_VERIFY_PIXEL(quarter, kW / 2, kH / 2, (Rgb{204, 0, 51}), 3);
    // aligned to start the window runs after the cut: at the cut the outgoing clip is still all there
    s.doc.transitions[0].align = QStringLiteral("start");
    SF_VERIFY_PIXEL(render(s, 20), kW / 2, kH / 2, (Rgb{255, 0, 0}), 2);
    SF_VERIFY_PIXEL(render(s, 25), kW / 2, kH / 2, (Rgb{128, 0, 128}), 3);
    // a transition may be removed again: back to a hard cut
    s.doc.transitions.clear();
    SF_VERIFY_PIXEL(render(s, 17), kW / 2, kH / 2, (Rgb{255, 0, 0}), 1);
  }

  void wipeSplitsTheFrame() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = redThenBlue();
    // left to right: at 50% the left half is the incoming (blue) picture, the right half the outgoing (red)
    s.doc.transitions = {transition(QStringLiteral("wipe"), 10, QJsonObject{{Q("softness"), 0.01}})};
    const QImage img = render(s, 20);
    SF_VERIFY_PIXEL(img, 16, kH / 2, (Rgb{0, 0, 255}), 2);
    SF_VERIFY_PIXEL(img, 16, 5, (Rgb{0, 0, 255}), 2);
    SF_VERIFY_PIXEL(img, kW - 16, kH / 2, (Rgb{255, 0, 0}), 2);
    SF_VERIFY_PIXEL(img, kW - 16, kH - 5, (Rgb{255, 0, 0}), 2);
    // direction (0, 1) is up: the incoming picture rises from the bottom
    s.doc.transitions[0].params = QJsonObject{{Q("direction"), QJsonArray{0, 1}}, {Q("softness"), 0.01}};
    const QImage up = render(s, 20);
    SF_VERIFY_PIXEL(up, kW / 2, kH - 8, (Rgb{0, 0, 255}), 2);
    SF_VERIFY_PIXEL(up, kW / 2, 8, (Rgb{255, 0, 0}), 2);
    // the edge advances with progress
    s.doc.transitions[0].params = QJsonObject{{Q("softness"), 0.01}};
    SF_VERIFY_PIXEL(render(s, 17), 16, kH / 2, (Rgb{0, 0, 255}), 2);   // 20%: edge at x = 25
    SF_VERIFY_PIXEL(render(s, 17), 60, kH / 2, (Rgb{255, 0, 0}), 2);
    SF_VERIFY_PIXEL(render(s, 23), 100, kH / 2, (Rgb{0, 0, 255}), 2);  // 80%: edge at x = 102
    SF_VERIFY_PIXEL(render(s, 23), 120, kH / 2, (Rgb{255, 0, 0}), 2);
  }

  // ---- gl-transitions port: every gl_ transition, both ends ----
  void glTransitionsEnds_data() {
    QTest::addColumn<QString>("id");
    for (const FxDef& d : FxRegistry::instance().all())
      if (d.kind == FxKind::Transition && d.id.startsWith(QLatin1String("gl_"))) QTest::newRow(qPrintable(d.id)) << d.id;
  }

  // A pipeline per transition (a shader or pipeline failure shows as fxErrors), rendered at progress 0, 0.5 and 0.99 between a red
  // and a blue clip. At the ends the picture must be the outgoing / incoming clip, on average: some transitions legitimately draw
  // something else there (a flash, a border, a letterbox, a flip that has not finished), they are listed below.
  void glTransitionsEnds() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    QFETCH(QString, id);
    Scene s;
    s.doc = newDoc(kW, kH);
    const QString track = addTopTrack(s.doc, QStringLiteral("trk_gfx"), TrackKind::Overlay);
    solid(s, QStringLiteral("itm_red"), track, QStringLiteral("#ff0000"), 0, 200);
    solid(s, QStringLiteral("itm_blue"), track, QStringLiteral("#0000ff"), 200, 200);
    blendIn(s, BlendSpace::Display);
    s.doc.transitions = {transition(id, 100)}; // window 150..250, progress (f - 150) / 100

    // mean absolute channel difference to a flat colour, 0..255
    const auto distance = [](const QImage& img, int r, int g, int b) {
      const QImage px = img.convertToFormat(QImage::Format_RGBA8888);
      double sum = 0;
      for (int y = 0; y < px.height(); ++y)
        for (int x = 0; x < px.width(); ++x) {
          const QRgb c = px.pixel(x, y);
          sum += qAbs(qRed(c) - r) + qAbs(qGreen(c) - g) + qAbs(qBlue(c) - b);
        }
      return sum / (3.0 * px.width() * px.height());
    };

    const std::array<Frame, 3> frames{150, 200, 249};
    double atStart = 0, atEnd = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
      RenderStats stats;
      const QImage img = render(s, frames[i], &stats);
      QVERIFY2(!img.isNull(), qPrintable(id));
      QVERIFY2(stats.fxErrors == 0, qPrintable(QStringLiteral("%1: shader or pipeline failure at frame %2").arg(id).arg(frames[i])));
      QCOMPARE(stats.fxPasses, 1);
      if (i == 0) atStart = distance(img, 255, 0, 0);
      if (i == 2) atEnd = distance(img, 0, 0, 255);
    }
    // transitions that do not start on the outgoing picture or end on the incoming one (see the report in the log)
    static const QSet<QString> startsElsewhere = {};
    static const QSet<QString> endsElsewhere = {};
    constexpr double kTolerance = 25;
    if (!startsElsewhere.contains(id)) QVERIFY2(atStart < kTolerance, qPrintable(QStringLiteral("%1: progress 0 is %2 away from the outgoing colour").arg(id).arg(atStart)));
    if (!endsElsewhere.contains(id)) QVERIFY2(atEnd < kTolerance, qPrintable(QStringLiteral("%1: progress 0.99 is %2 away from the incoming colour").arg(id).arg(atEnd)));
  }

  // ---- the filter pack (light, lens, retro, stylize, motion) ----
  void filterPackRenders_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<int>("passes");
    for (const FxDef& d : FxRegistry::instance().all()) {
      if (d.kind != FxKind::Filter) continue;
      if (d.id == QLatin1String("vignette") || d.id == QLatin1String("gaussian_blur") || d.id == QLatin1String("chromatic_aberration")) continue;
      QTest::newRow(qPrintable(d.id)) << d.id << d.passes;
    }
  }

  // Every new filter at its default params: no pipeline error, the right number of passes, a picture that differs from the
  // input, and the same pixels again for the same frame (time and seed).
  void filterPackRenders() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    QFETCH(QString, id);
    QFETCH(int, passes);
    Scene s = still(pattern_);
    const QImage plain = render(s, 7);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_p"), id)};
    RenderStats stats;
    const QImage a = render(s, 7, &stats);
    QCOMPARE(stats.fxErrors, 0);
    QCOMPARE(stats.fxPasses, passes);
    QVERIFY2(maxAbsDiff(plain, a) >= 2, qPrintable(id + QStringLiteral(" left the picture unchanged")));
    QCOMPARE(maxAbsDiff(a, render(s, 7)), 0);
  }

  void filterPackIsDescribed() {
    int n = 0;
    for (const FxDef& d : FxRegistry::instance().all()) n += d.kind == FxKind::Filter ? 1 : 0;
    QVERIFY2(n >= 28, qPrintable(QString::number(n)));
    for (const char* id : {"glow", "light_leak", "lens_flare_simple", "god_rays", "lens_distortion", "radial_blur", "directional_blur", "tilt_shift", "sharpen", "defocus",
                           "film_grain", "vhs", "crt", "halftone", "posterize", "pixelate", "dither", "mirror", "kaleidoscope", "duotone", "invert", "threshold",
                           "edge_detect", "rgb_split_glitch", "wave_warp", "swirl", "camera_shake"}) {
      const FxDef* d = FxRegistry::instance().find(Q(id));
      QVERIFY2(d, id);
      QVERIFY2(!d->description.isEmpty() && !d->tags.isEmpty() && !d->whenToUse.isEmpty(), id);
      QVERIFY2(d->category == Q("light") || d->category == Q("lens") || d->category == Q("retro") || d->category == Q("stylize") || d->category == Q("motion"), id);
    }
  }

  void seedAndTimeChangeTheRandomLooks() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    for (const char* id : {"film_grain", "rgb_split_glitch", "camera_shake", "vhs"}) {
      s.doc.items[0].fx = {fx(QStringLiteral("fx_p"), Q(id), QJsonObject{{Q("seed"), 1}})};
      const QImage a = render(s, 7);
      s.doc.items[0].fx = {fx(QStringLiteral("fx_p"), Q(id), QJsonObject{{Q("seed"), 2}})};
      QVERIFY2(maxAbsDiff(a, render(s, 7)) > 0, id);
      QVERIFY2(maxAbsDiff(render(s, 7), render(s, 19)) > 0, id);
    }
  }

  void invertTwiceIsTheOriginal() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    const QImage plain = render(s, 0);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_a"), QStringLiteral("invert")), fx(QStringLiteral("fx_b"), QStringLiteral("invert"))};
    QVERIFY2(maxAbsDiff(plain, render(s, 0)) <= 3, "invert o invert");
    s.doc.items[0].fx.pop_back();
    QVERIFY(maxAbsDiff(plain, render(s, 0)) > 100);
  }

  void thresholdIsOnlyBlackAndWhite() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_t"), QStringLiteral("threshold"))};
    const QImage img = render(s, 0).convertToFormat(QImage::Format_RGBA8888);
    int whites = 0, blacks = 0;
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x) {
        const QRgb c = img.pixel(x, y);
        for (int v : {qRed(c), qGreen(c), qBlue(c)}) QVERIFY2(v == 0 || v == 255, qPrintable(describe(c)));
        (qRed(c) ? whites : blacks)++;
      }
    QVERIFY(whites > 500 && blacks > 500);
  }

  void posterizeLimitsTheLevels() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_p"), QStringLiteral("posterize"), QJsonObject{{Q("levels"), 4}})};
    const QImage img = render(s, 0).convertToFormat(QImage::Format_RGBA8888);
    std::set<int> reds, blues;
    for (int y = 0; y < kH / 2; ++y) // the gradient half: red climbs through every level, blue falls through them
      for (int x = 0; x < kW; ++x) {
        reds.insert(qRed(img.pixel(x, y)));
        blues.insert(qBlue(img.pixel(x, y)));
      }
    QCOMPARE(int(reds.size()), 4);
    QCOMPARE(int(blues.size()), 4);
  }

  void pixelateMakesConstantBlocks() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_p"), QStringLiteral("pixelate"), QJsonObject{{Q("size"), 8.0}})};
    const QImage img = render(s, 0).convertToFormat(QImage::Format_RGBA8888);
    for (int by = 0; by < kH / 8; ++by)
      for (int bx = 0; bx < kW / 8; ++bx) {
        const QRgb first = img.pixel(bx * 8, by * 8);
        for (int y = 0; y < 8; y += 2)
          for (int x = 0; x < 8; x += 2) QVERIFY2(closeTo(img.pixel(bx * 8 + x, by * 8 + y), Rgb{qRed(first), qGreen(first), qBlue(first)}, 2), "block not constant");
      }
  }

  void mirrorIsSymmetric() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(pattern_);
    const QImage plain = render(s, 0).convertToFormat(QImage::Format_RGBA8888);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_m"), QStringLiteral("mirror"))};
    const QImage img = render(s, 0).convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < kH; y += 5)
      for (int x = 0; x < kW / 2; x += 3) {
        const QRgb l = img.pixel(x, y), r = img.pixel(kW - 1 - x, y), p = plain.pixel(x, y);
        QVERIFY2(closeTo(r, Rgb{qRed(l), qGreen(l), qBlue(l)}, 3), qPrintable(QStringLiteral("(%1,%2) %3 vs %4").arg(x).arg(y).arg(describe(l), describe(r))));
        QVERIFY2(closeTo(l, Rgb{qRed(p), qGreen(p), qBlue(p)}, 3), "kept half changed");
      }
  }

  void duotoneHitsItsColoursAtTheExtremes() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(halves_);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_d"), QStringLiteral("duotone"),
                            QJsonObject{{Q("amount"), 1.0}, {Q("shadow"), Q("#102030")}, {Q("highlight"), Q("#f0e0a0")}})};
    const QImage img = render(s, 0);
    SF_VERIFY_PIXEL(img, 10, 10, (Rgb{0xf0, 0xe0, 0xa0}), 3);
    SF_VERIFY_PIXEL(img, 120, 60, (Rgb{0x10, 0x20, 0x30}), 3);
  }

  void glowAddsLightAroundBrightEdges() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = still(halves_);
    const QImage plain = render(s, 0);
    s.doc.items[0].fx = {fx(QStringLiteral("fx_g"), QStringLiteral("glow"), QJsonObject{{Q("radius"), 0.1}, {Q("intensity"), 1.0}})};
    const QImage img = render(s, 0);
    QVERIFY2(qRed(img.pixel(kW / 2 + 4, kH / 2)) > qRed(plain.pixel(kW / 2 + 4, kH / 2)) + 20, "no glow on the dark side of the edge");
    SF_VERIFY_PIXEL(img, kW - 3, kH / 2, (Rgb{0, 0, 0}), 40); // far away stays dark
  }

  void clipEffectsRunInsideATransition() {
    if (!gpu_) QSKIP(qPrintable(skip_));
    Scene s = redThenBlue();
    s.doc.transitions = {transition(QStringLiteral("crossfade"), 10)};
    s.doc.items[0].fx = {fx(QStringLiteral("fx_v"), QStringLiteral("vignette"), QJsonObject{{Q("amount"), 1.0}, {Q("radius"), 0.3}})};
    RenderStats stats;
    const QImage img = render(s, 20, &stats);
    QCOMPARE(stats.fxPasses, 2); // the vignette on the outgoing clip, then the mix
    // centre: half red (undarkened) + half blue; corner: only the blue half survives
    SF_VERIFY_PIXEL(img, kW / 2, kH / 2, (Rgb{128, 0, 128}), 3);
    const QRgb corner = img.pixel(1, 1);
    QVERIFY2(qRed(corner) < 20 && qBlue(corner) > 110 && qBlue(corner) < 145, qPrintable(describe(corner)));
  }
};

QTEST_MAIN(TstEffects)
#include "tst_effects.moc"
