// Compositing maths that needs no GPU: layer order, keyframes, fades, caption cards, placement,
// colour matrices. Cross-checked against packages/renderer behaviour (keyframes.ts, captions.ts, effects.ts).

#include "render/layout.h"
#include "test_helpers.h"

#include <QTest>

using namespace sf;
using namespace sf::render;

namespace {

Item videoAt(const QString& id, const QString& track, Frame start, Frame dur) {
  ItemInit init;
  init.id = id;
  init.trackId = track;
  init.startFrame = start;
  init.durationFrames = dur;
  init.assetId = QStringLiteral("ast_x");
  init.sourceInFrame = 0;
  return createItem(ItemType::Video, init);
}

std::array<float, 3> apply(const ColorMatrix& m, float a, float b, float c) {
  std::array<float, 3> out{};
  for (size_t i = 0; i < 3; ++i) out[i] = m[i][0] * a + m[i][1] * b + m[i][2] * c + m[i][3];
  return out;
}

} // namespace

class TstLayout : public QObject {
  Q_OBJECT

private slots:
  void drawOrderIsBottomTrackFirst() {
    TimelineDoc doc = test::makeDoc(); // tracks: text (top), main, audio
    doc.items.push_back(videoAt(QStringLiteral("itm_main"), trackOfKindId(doc, TrackKind::Video), 0, 100));
    Track top;
    top.id = QStringLiteral("trk_top");
    top.kind = TrackKind::Video;
    doc.tracks.insert(doc.tracks.begin(), top);
    doc.items.push_back(videoAt(QStringLiteral("itm_top"), top.id, 0, 100));
    const auto active = activeItems(doc, 10);
    QCOMPARE(active.size(), size_t(2));
    QCOMPARE(active[0]->id, QStringLiteral("itm_main")); // drawn first = underneath
    QCOMPARE(active[1]->id, QStringLiteral("itm_top"));
  }

  void itemsAreActiveForTheirIntervalOnly() {
    TimelineDoc doc = test::makeDoc();
    doc.items.push_back(videoAt(QStringLiteral("itm_a"), trackOfKindId(doc, TrackKind::Video), 10, 20));
    QCOMPARE(activeItems(doc, 9).size(), size_t(0));
    QCOMPARE(activeItems(doc, 10).size(), size_t(1));
    QCOMPARE(activeItems(doc, 29).size(), size_t(1));
    QCOMPARE(activeItems(doc, 30).size(), size_t(0));
  }

  void hiddenTracksAreSkipped() {
    TimelineDoc doc = test::makeDoc();
    doc.items.push_back(videoAt(QStringLiteral("itm_a"), trackOfKindId(doc, TrackKind::Video), 0, 20));
    for (Track& t : doc.tracks) t.hidden = true;
    QCOMPARE(activeItems(doc, 5).size(), size_t(0));
  }

  void sameTrackItemsOrderByStartThenId() {
    TimelineDoc doc = test::makeDoc();
    const QString trk = trackOfKindId(doc, TrackKind::Video);
    doc.items.push_back(videoAt(QStringLiteral("itm_b"), trk, 0, 50));
    doc.items.push_back(videoAt(QStringLiteral("itm_a"), trk, 0, 50));
    doc.items.push_back(videoAt(QStringLiteral("itm_0"), trk, 5, 50));
    const auto active = activeItems(doc, 10);
    QCOMPARE(active.size(), size_t(3));
    QCOMPARE(active[0]->id, QStringLiteral("itm_a"));
    QCOMPARE(active[1]->id, QStringLiteral("itm_b"));
    QCOMPARE(active[2]->id, QStringLiteral("itm_0"));
  }

  void keyframeInterpolation() {
    const KeyframeList kfs{{10, 0.0, Easing::Linear}, {20, 100.0, Easing::Linear}, {30, 0.0, Easing::Hold}};
    QCOMPARE(resolveKeyframes(kfs, 0), 0.0);    // before the first: holds it
    QCOMPARE(resolveKeyframes(kfs, 15), 50.0);
    QCOMPARE(resolveKeyframes(kfs, 20), 100.0);
    QCOMPARE(resolveKeyframes(kfs, 25), 100.0); // hold easing on the next keyframe keeps the previous value
    QCOMPARE(resolveKeyframes(kfs, 30), 0.0);
    QCOMPARE(resolveKeyframes(kfs, 99), 0.0);   // after the last
    const KeyframeList eased{{0, 0.0, Easing::Linear}, {10, 100.0, Easing::EaseIn}};
    QCOMPARE(resolveKeyframes(eased, 5), 25.0); // t*t
    const KeyframeList out{{0, 0.0, Easing::Linear}, {10, 100.0, Easing::EaseOut}};
    QCOMPARE(resolveKeyframes(out, 5), 75.0);
    const KeyframeList inOut{{0, 0.0, Easing::Linear}, {10, 100.0, Easing::EaseInOut}};
    QCOMPARE(resolveKeyframes(inOut, 2), 100.0 * 2 * 0.2 * 0.2);
    // unsorted input is sorted first
    const KeyframeList unsorted{{20, 100.0, Easing::Linear}, {10, 0.0, Easing::Linear}};
    QCOMPARE(resolveKeyframes(unsorted, 15), 50.0);
  }

  void propertyFallsBackToTheStaticValueAndUsesLocalTime() {
    Item item = videoAt(QStringLiteral("itm_a"), QStringLiteral("trk"), 100, 50);
    item.transform.x = 7;
    QCOMPARE(resolveProperty(item, QStringLiteral("transform.x"), 120), 7.0);
    item.keyframes[QStringLiteral("transform.x")] = {{0, 0.0, Easing::Linear}, {10, 10.0, Easing::Linear}};
    QCOMPARE(resolveProperty(item, QStringLiteral("transform.x"), 105), 5.0); // item-local frame 5
    QCOMPARE(resolveProperty(item, QStringLiteral("transform.scale"), 105), 1.0);
  }

  void opacityWithFades() {
    Item item = videoAt(QStringLiteral("itm_a"), QStringLiteral("trk"), 100, 50);
    item.props = VideoProps{{10, 20}};
    item.transform.opacity = 0.8;
    QCOMPARE(opacityAt(item, 100), 0.0);
    QCOMPARE(opacityAt(item, 105), 0.4);
    QCOMPARE(opacityAt(item, 110), 0.8);
    QCOMPARE(opacityAt(item, 120), 0.8);
    QCOMPARE(opacityAt(item, 140), 0.4); // 10 frames from the end of a 20 frame fade
    QCOMPARE(opacityAt(item, 149), 0.8 * 1.0 / 20);
    item.transform.opacity = 3; // clamped
    QCOMPARE(opacityAt(item, 120), 1.0);
  }

  void captionClockAndCards() {
    QCOMPARE(captionClockMs(45, 15, 30), Ms(1000));
    QCOMPARE(captionClockMs(0, 15, 30), Ms(-500));
    const std::vector<TranscriptWord> words{{QStringLiteral("a"), 0, 100, {}, {}}, {QStringLiteral("b"), 100, 200, {}, {}}, {QStringLiteral("c"), 300, 400, {}, {}},
                                            {QStringLiteral("d"), 400, 500, {}, {}}, {QStringLiteral("e"), 500, 600, {}, {}}};
    QVERIFY(!captionCardAt(words, -1, 2));
    const auto first = captionCardAt(words, 50, 2);
    QVERIFY(first);
    QCOMPARE(first->words.size(), size_t(2));
    QCOMPARE(first->activeIdx, 0);
    QCOMPARE(first->words[0]->w, QStringLiteral("a"));
    // between words the card of the most recent word stays up
    const auto gap = captionCardAt(words, 250, 2);
    QVERIFY(gap);
    QCOMPARE(gap->activeIdx, -1);
    QCOMPARE(gap->words[0]->w, QStringLiteral("a"));
    const auto third = captionCardAt(words, 420, 2);
    QVERIFY(third);
    QCOMPARE(third->words[0]->w, QStringLiteral("c"));
    QCOMPARE(third->activeIdx, 3);
    const auto last = captionCardAt(words, 550, 2);
    QCOMPARE(last->words.size(), size_t(1)); // the partial last card
    QCOMPARE(last->words[0]->w, QStringLiteral("e"));
    QCOMPARE(captionCardAt(words, 50, 0)->words.size(), size_t(1)); // maxWordsPerCard is at least 1
  }

  void placementFitsThenTransforms() {
    Item item = videoAt(QStringLiteral("itm_a"), QStringLiteral("trk"), 0, 10);
    // 4:3 content in a 16:9 canvas: contain -> 480x360
    Placement p = placeMedia(item, 0, QSizeF(320, 240), QSize(640, 360));
    QCOMPARE(p.size, QSizeF(480, 360));
    QCOMPARE(p.center, QPointF(320, 180));
    QCOMPARE(p.unitToCanvas().map(QPointF(0, 0)), QPointF(80, 0));
    QCOMPARE(p.unitToCanvas().map(QPointF(1, 1)), QPointF(560, 360));
    item.transform.scale = 0.5;
    item.transform.scaleX = 2; // x ends up 1.0, y 0.5
    item.transform.x = 100;
    item.transform.y = -30;
    p = placeMedia(item, 0, QSizeF(320, 240), QSize(640, 360));
    QCOMPARE(p.center, QPointF(420, 150));
    QCOMPARE(p.scaleX, 1.0);
    QCOMPARE(p.scaleY, 0.5);
    const QTransform t = p.unitToCanvas();
    QCOMPARE(t.map(QPointF(0, 0)), QPointF(420 - 240, 150 - 90));
    QCOMPARE(t.map(QPointF(1, 1)), QPointF(420 + 240, 150 + 90));
    // 90 degrees clockwise (y down): the unit square's top-left goes to the top-right
    item.transform.scaleX = 1;
    item.transform.scale = 1;
    item.transform.x = item.transform.y = 0;
    item.transform.rotation = 90;
    p = placeMedia(item, 0, QSizeF(300, 300), QSize(300, 300));
    const QPointF tl = p.unitToCanvas().map(QPointF(0, 0));
    QVERIFY(std::abs(tl.x() - 300) < 1e-9 && std::abs(tl.y() - 0) < 1e-9);
  }

  void yuvMatricesHitTheirReferencePoints() {
    // BT.709 limited range: Y=16 is black, 235 white, chroma 128 is neutral
    const ColorMatrix m709 = yuvToRgb(0.2126, 0.0722, false, 8);
    const auto black = apply(m709, 16 / 255.f, 128 / 255.f, 128 / 255.f);
    const auto white = apply(m709, 235 / 255.f, 128 / 255.f, 128 / 255.f);
    for (int c = 0; c < 3; ++c) {
      QVERIFY(std::abs(black[size_t(c)]) < 0.002f);
      QVERIFY(std::abs(white[size_t(c)] - 1) < 0.002f);
    }
    // pure red in BT.709: Y=63 (0.2126*219+16), Cb=102, Cr=240
    const auto red = apply(m709, 63.0f / 255, 102.0f / 255, 240.0f / 255);
    QVERIFY(std::abs(red[0] - 1) < 0.01f && red[1] < 0.01f && red[2] < 0.01f);
    // BT.601 gives something else for the same samples (this is what the stream tag decides)
    const ColorMatrix m601 = yuvToRgb(0.299, 0.114, false, 8);
    const auto red601 = apply(m601, 63.0f / 255, 102.0f / 255, 240.0f / 255);
    QVERIFY(std::abs(red601[1] - red[1]) > 0.01f || std::abs(red601[0] - red[0]) > 0.01f);
    // full range: 0 is black, 255 white, no 16..235 squeeze
    const ColorMatrix full = yuvToRgb(0.2126, 0.0722, true, 8);
    const auto fw = apply(full, 1, 128 / 255.f, 128 / 255.f);
    const auto fb = apply(full, 0, 128 / 255.f, 128 / 255.f);
    QVERIFY(std::abs(fw[0] - 1) < 0.003f && std::abs(fb[0]) < 0.003f);
    // P010: 10-bit codes live in the top of 16 bits; Y=940 (white) is 940*64/65535 of the range
    const ColorMatrix p010 = yuvToRgb(0.2126, 0.0722, false, 10);
    const float y940 = 940.f * 64 / 65535, c512 = 512.f * 64 / 65535, y64 = 64.f * 64 / 65535;
    const auto w10 = apply(p010, y940, c512, c512);
    const auto b10 = apply(p010, y64, c512, c512);
    for (int c = 0; c < 3; ++c) {
      QVERIFY(std::abs(w10[size_t(c)] - 1) < 0.002f);
      QVERIFY(std::abs(b10[size_t(c)]) < 0.002f);
    }
  }

  void rotationMapsDisplayToStoredCoords() {
    const auto map = [](int rot, float u, float v) {
      const UvMap m = uvForRotation(rot);
      return QPointF(m.x[0] * u + m.x[1] * v + m.x[2], m.y[0] * u + m.y[1] * v + m.y[2]);
    };
    QCOMPARE(map(0, 0.25f, 0.75f), QPointF(0.25, 0.75));
    // displayed top-left of a frame turned 90 cw is stored bottom-left
    QCOMPARE(map(90, 0, 0), QPointF(0, 1));
    QCOMPARE(map(90, 1, 0), QPointF(0, 0)); // displayed top-right = stored top-left
    QCOMPARE(map(180, 0, 0), QPointF(1, 1));
    QCOMPARE(map(270, 0, 0), QPointF(1, 0));
    QCOMPARE(map(270, 0, 1), QPointF(0, 0)); // displayed bottom-left = stored top-left
    QCOMPARE(map(-90, 0, 0), map(270, 0, 0));
  }

  void effectMatrices() {
    Effect e;
    e.id = QStringLiteral("fx");
    e.type = QStringLiteral("brightness");
    e.params[QStringLiteral("amount")] = 0.5;
    auto out = apply(effectsToColorMatrix({e}), 0.4f, 0.2f, 0.0f);
    QVERIFY(std::abs(out[0] - 0.6f) < 1e-5f && std::abs(out[1] - 0.3f) < 1e-5f);
    e.type = QStringLiteral("contrast");
    e.params.clear();
    e.params[QStringLiteral("amount")] = 2.0;
    out = apply(effectsToColorMatrix({e}), 0.75f, 0.5f, 0.25f);
    QVERIFY(std::abs(out[0] - 1.0f) < 1e-5f && std::abs(out[1] - 0.5f) < 1e-5f && std::abs(out[2] - 0.0f) < 1e-5f);
    e.type = QStringLiteral("grayscale");
    e.params.clear();
    e.params[QStringLiteral("amount")] = 1.0;
    out = apply(effectsToColorMatrix({e}), 1, 0, 0);
    QVERIFY(std::abs(out[0] - 0.2126f) < 1e-4f && std::abs(out[1] - 0.2126f) < 1e-4f && std::abs(out[2] - 0.2126f) < 1e-4f);
    e.type = QStringLiteral("saturation");
    e.params.clear();
    e.params[QStringLiteral("amount")] = 1.0; // identity
    out = apply(effectsToColorMatrix({e}), 0.1f, 0.5f, 0.9f);
    QVERIFY(std::abs(out[0] - 0.1f) < 1e-4f && std::abs(out[2] - 0.9f) < 1e-4f);
    e.type = QStringLiteral("hueRotate");
    e.params.clear();
    e.params[QStringLiteral("degrees")] = 0.0; // identity, no matrix step
    QCOMPARE(effectsToColorMatrix({e}), kIdentityColor);
    // stacking composes in order: brightness 2x then contrast 0 -> flat mid grey
    Effect b;
    b.id = QStringLiteral("b");
    b.type = QStringLiteral("brightness");
    b.params[QStringLiteral("amount")] = 1.0;
    Effect c;
    c.id = QStringLiteral("c");
    c.type = QStringLiteral("contrast");
    c.params[QStringLiteral("amount")] = 0.0;
    out = apply(effectsToColorMatrix({b, c}), 0.2f, 0.3f, 0.4f);
    QVERIFY(std::abs(out[0] - 0.5f) < 1e-5f);
    Effect blur;
    blur.id = QStringLiteral("blur");
    blur.type = QStringLiteral("blur");
    blur.params[QStringLiteral("radiusPx")] = 4.0;
    bool hasBlur = false;
    QCOMPARE(effectsToColorMatrix({blur}, &hasBlur), kIdentityColor);
    QVERIFY(hasBlur);
  }

private:
  static QString trackOfKindId(const TimelineDoc& doc, TrackKind kind) {
    for (const Track& t : doc.tracks) {
      if (t.kind == kind) return t.id;
    }
    return {};
  }
};

QTEST_GUILESS_MAIN(TstLayout)
#include "tst_layout.moc"
