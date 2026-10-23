// Deterministic delivery-safety checks: safety.check (photosensitive flashes, text legibility, platform safe zones).
// Every number comes from measured pixels or from the document; thresholds are reported next to each measurement and fixes are data only.
// Pure maths lives in safety_analysis.h; this file renders the range at reduced resolution and reads the schema.

#include "engine/cmd_util.h"
#include "engine/safety_analysis.h"
#include "media/frame_service.h"
#include "render/colour_capture.h"
#include "render/layer_painter.h"
#include "render/layout.h"
#include "render/media_provider.h"
#include "render/offscreen.h"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace sf::engine {

namespace {

using namespace safety;

double r3(double v) { return std::round(v * 1000.0) / 1000.0; }
double fpsOf(const TimelineDoc& d) { return static_cast<double>(d.project.fps > 0 ? d.project.fps : 30); }

struct Out {
  QJsonArray violations, warnings, info;
  QJsonObject details;
  QStringList skipped;
  void add(const QString& severity, const QJsonObject& e) {
    if (severity == QLatin1String("violation")) violations.append(e);
    else if (severity == QLatin1String("warning")) warnings.append(e);
    else info.append(e);
  }
};

QJsonObject entry(const QString& check, const QString& kind, double startSec, double endSec, const QJsonObject& measured, const QJsonObject& threshold, const QJsonObject& fix,
                  const QString& message, const QString& itemId = {}) {
  QJsonObject o{{q("check"), check}, {q("kind"), kind}, {q("startSec"), r3(startSec)}, {q("endSec"), r3(endSec)}, {q("measured"), measured}, {q("threshold"), threshold}, {q("message"), message}};
  if (!itemId.isEmpty()) o.insert(q("itemId"), itemId);
  if (!fix.isEmpty()) o.insert(q("fix"), fix);
  return o;
}

// ---------------------------------------------------------------- rendering at reduced size

void scaleStyle(TextStyle& s, double k) {
  s.fontSize *= k;
  s.strokeWidth *= k;
  s.padding *= k;
  s.borderRadius *= k;
  s.letterSpacing *= k;
}

// The doc on a W x H canvas: sizes and offsets that are canvas pixels scale (static values only; keyframed x / y are left alone).
TimelineDoc scaledDoc(const TimelineDoc& d, int W, int H) {
  TimelineDoc o = d;
  const double k = static_cast<double>(W) / static_cast<double>(std::max<std::int64_t>(1, d.project.width));
  o.project.width = W;
  o.project.height = H;
  for (Item& it : o.items) {
    it.transform.x *= k;
    it.transform.y *= k;
    if (auto* t = std::get_if<TextProps>(&it.props)) scaleStyle(t->style, k);
    else if (auto* cp = std::get_if<CaptionProps>(&it.props)) scaleStyle(cp->style, k);
    else if (auto* s = std::get_if<ShapeProps>(&it.props)) {
      s->width *= k;
      s->height *= k;
      s->strokeWidth *= k;
      s->radius *= k;
    }
  }
  return o;
}

const std::array<float, 256>& srgbLut() {
  static const std::array<float, 256> lut = [] {
    std::array<float, 256> l{};
    for (int i = 0; i < 256; ++i) l[static_cast<size_t>(i)] = static_cast<float>(srgbToLinear(i / 255.0));
    return l;
  }();
  return lut;
}

// renderFrames moved to cmd_util.{h,cpp}

// ---------------------------------------------------------------- photosensitive

QJsonObject flashFix(const char* what, double fps, double allowedFlashes) {
  const int minSpacing = static_cast<int>(std::ceil(fps / (2.0 * allowedFlashes)));
  return QJsonObject{{q("suggest"), QStringLiteral("Slow the cut rate or lower the contrast of the %1 in this window.").arg(QString::fromLatin1(what))},
                     {q("options"), QJsonArray{QJsonObject{{q("type"), q("slow_cut_rate")}, {q("minFramesBetweenChanges"), minSpacing}, {q("maxFlashesPerSecond"), allowedFlashes}},
                                               QJsonObject{{q("type"), q("lower_contrast")}, {q("maxLuminanceDelta"), 0.1}}}}};
}

void checkPhotosensitive(CallContext& c, const TimelineDoc& doc, Frame rs, Frame re, int analysisWidth, int maxFrames, Out& out) {
  const double fps = fpsOf(doc);
  const int fpsI = static_cast<int>(std::lround(fps));
  const FlashParams fp;
  QJsonObject det;
  if (re - rs > maxFrames) {
    out.info.append(entry(q("photosensitive"), q("truncated"), rs / fps, re / fps, QJsonObject{{q("frames"), static_cast<double>(re - rs)}}, QJsonObject{{q("maxFrames"), maxFrames}}, {},
                         QStringLiteral("The range holds %1 frames; only the first %2 were analysed (raise maxFrames or use `range`).").arg(re - rs).arg(maxFrames)));
    re = rs + maxFrames;
  }
  if (re - rs < 2) {
    out.skipped << q("photosensitive: fewer than 2 frames in range");
    return;
  }
  const int W = std::clamp(analysisWidth, 16, static_cast<int>(std::max<std::int64_t>(16, doc.project.width)));
  const int H = std::max(8, static_cast<int>(std::lround(static_cast<double>(W) * static_cast<double>(doc.project.height) / static_cast<double>(std::max<std::int64_t>(1, doc.project.width)))));
  const TimelineDoc small = scaledDoc(doc, W, H);
  FlashDetector det8(W, H, fp);
  std::vector<float> buf(static_cast<size_t>(W) * static_cast<size_t>(H) * 3);
  std::vector<Frame> frames;
  for (Frame f = rs; f < re; ++f) frames.push_back(f);
  const auto& lut = srgbLut();
  int framesSeen = 0;
  const Result r = renderFrames(c, small, frames, [&](Frame f, const QImage& img) {
    if (img.width() != W || img.height() != H) return;
    for (int y = 0; y < H; ++y) {
      const uchar* row = img.constScanLine(y);
      float* dst = buf.data() + static_cast<size_t>(y) * static_cast<size_t>(W) * 3;
      for (int x = 0; x < W; ++x) {
        dst[x * 3] = lut[row[x * 4]];
        dst[x * 3 + 1] = lut[row[x * 4 + 1]];
        dst[x * 3 + 2] = lut[row[x * 4 + 2]];
      }
    }
    det8.push(buf.data(), f);
    ++framesSeen;
  });
  if (!r.ok) {
    out.skipped << QStringLiteral("photosensitive: %1").arg(r.error.message);
    return;
  }

  const QJsonObject thr{{q("maxFlashesPerSecond"), fp.maxFlashesPerSecond}, {q("windowSec"), 1}, {q("minLuminanceDelta"), fp.minLumaDelta}, {q("darkerStateBelow"), fp.darkerBelow},
                        {q("minAreaFraction"), r3(fp.minAreaFraction)}};
  const auto handle = [&](const char* name, const std::vector<Frame>& ev, double worstArea, const char* what) {
    const WindowSummary ws = summarize(ev, rs, re, fpsI, fp.maxFlashesPerSecond);
    QJsonArray per;
    for (size_t s = 0; s < ws.perSecond.size(); ++s) per.append(QJsonObject{{q("startSec"), r3((rs + static_cast<Frame>(s) * fpsI) / fps)}, {q("flashes"), ws.perSecond[s]}});
    det.insert(q(name), QJsonObject{{q("flashesPerSecondWindow"), per}, {q("worstWindow"), QJsonObject{{q("startSec"), r3(ws.worstStart / fps)}, {q("endSec"), r3((ws.worstStart + fpsI) / fps)}, {q("flashes"), ws.worstFlashes}}},
                                    {q("changes"), static_cast<double>(ev.size())}, {q("largestChangedAreaFraction"), r3(worstArea)}});
    for (const auto& span : ws.violating) {
      const int changes = static_cast<int>(std::count_if(ev.begin(), ev.end(), [&](Frame f) { return f >= span.first && f <= span.second; }));
      const QString kind = QStringLiteral("photosensitive_%1_flash").arg(QString::fromLatin1(name));
      out.violations.append(entry(q("photosensitive"), kind, span.first / fps, (span.second + 1) / fps,
                                  QJsonObject{{q("worstWindowFlashes"), ws.worstFlashes}, {q("worstWindowStartSec"), r3(ws.worstStart / fps)}, {q("changesInSpan"), changes}, {q("largestChangedAreaFraction"), r3(worstArea)}}, thr,
                                  flashFix(what, fps, fp.maxFlashesPerSecond),
                                  QStringLiteral("More than %1 %2 flashes per second between %3 s and %4 s (worst window %5).")
                                      .arg(fp.maxFlashesPerSecond).arg(QString::fromLatin1(name)).arg(span.first / fps, 0, 'f', 2).arg((span.second + 1) / fps, 0, 'f', 2).arg(ws.worstFlashes)));
    }
  };
  handle("general", det8.generalEvents(), det8.worstGeneralArea(), "picture");
  handle("red", det8.redEvents(), det8.worstRedArea(), "red areas");
  QJsonArray patterns;
  for (const auto& run : runsOf(det8.patternFrames(), fp.patternMinFrames)) {
    patterns.append(QJsonObject{{q("startSec"), r3(run.first / fps)}, {q("endSec"), r3((run.second + 1) / fps)}});
    out.warnings.append(entry(q("photosensitive"), q("photosensitive_pattern"), run.first / fps, (run.second + 1) / fps, QJsonObject{{q("frames"), static_cast<double>(run.second - run.first + 1)}},
                              QJsonObject{{q("minLightDarkPairs"), fp.patternMinPairs}, {q("minSwing"), fp.patternSwing}},
                              QJsonObject{{q("suggest"), q("Reduce the contrast of the stripes or make them fewer and wider.")}},
                              q("Regular high-contrast stripes span the frame (best-effort column / row profile test).")));
  }
  det.insert(q("patternRuns"), patterns);
  det.insert(q("analysis"), QJsonObject{{q("width"), W}, {q("height"), H}, {q("frames"), framesSeen}, {q("fps"), fps}, {q("firstFrame"), static_cast<double>(rs)}, {q("lastFrame"), static_cast<double>(re - 1)},
                                         {q("method"), q("Per frame pair: pixels whose linear luminance moves >= 10 % with the darker state < 0.80; a change counts when its share of the whole frame area is >= minAreaFraction (WCAG 341x256 of 1024x768); "
                                                         "alternating changes in a 1 s sliding window give floor(N/2) flashes. Red flash: (R-G-B)*320 moves > 20 with a saturated-red state (R/(R+G+B) >= 0.8).")}});
  out.details.insert(q("photosensitive"), det);
}

// ---------------------------------------------------------------- text helpers

QColor parseCss(const QString& s, const QColor& fallback) {
  static const QRegularExpression rgb(QStringLiteral(R"(^\s*rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:[,\s/]+([\d.]+%?))?\s*\)\s*$)"));
  const QRegularExpressionMatch m = rgb.match(s);
  if (m.hasMatch()) {
    double a = 1;
    if (!m.captured(4).isEmpty()) a = m.captured(4).endsWith(u'%') ? m.captured(4).chopped(1).toDouble() / 100 : m.captured(4).toDouble();
    return QColor(static_cast<int>(std::lround(m.captured(1).toDouble())), static_cast<int>(std::lround(m.captured(2).toDouble())), static_cast<int>(std::lround(m.captured(3).toDouble())),
                  static_cast<int>(std::lround(std::clamp(a, 0.0, 1.0) * 255)));
  }
  if (s.startsWith(u'#') && s.size() == 9) { // CSS #RRGGBBAA
    bool ok = false;
    const uint v = s.mid(1).toUInt(&ok, 16);
    if (ok) return QColor(static_cast<int>((v >> 24) & 255), static_cast<int>((v >> 16) & 255), static_cast<int>((v >> 8) & 255), static_cast<int>(v & 255));
  }
  const QColor c(s);
  return c.isValid() ? c : fallback;
}

double colourLum(const QColor& c) { return relLuminance(srgbToLinear(c.redF()), srgbToLinear(c.greenF()), srgbToLinear(c.blueF())); }

const TextStyle* styleOf(const Item& it) {
  if (const auto* t = std::get_if<TextProps>(&it.props)) return &t->style;
  if (const auto* cp = std::get_if<CaptionProps>(&it.props)) return &cp->style;
  return nullptr;
}
bool isTextItem(const Item& it) { return it.type() == ItemType::Text || it.type() == ItemType::Caption; }

QString textOf(const Item& it) {
  if (const auto* t = std::get_if<TextProps>(&it.props)) return t->text;
  QStringList w;
  if (const auto* cp = std::get_if<CaptionProps>(&it.props))
    for (const TranscriptWord& x : cp->words) w << x.w;
  return w.join(u' ');
}

bool trackHidden(const TimelineDoc& d, const QString& id) {
  for (const Track& t : d.tracks)
    if (t.id == id) return t.hidden;
  return false;
}

bool inRange(const Item& it, Frame rs, Frame re) { return it.startFrame < re && it.startFrame + it.durationFrames > rs; }

std::vector<Frame> samplesOf(const Item& it, Frame rs, Frame re, int n) {
  const Frame a = std::max(it.startFrame, rs), b = std::min(it.startFrame + it.durationFrames, re);
  std::vector<Frame> out;
  for (int i = 0; i < n; ++i) {
    const Frame f = a + static_cast<Frame>((i + 0.5) * static_cast<double>(b - a) / n);
    out.push_back(std::clamp(f, a, std::max(a, b - 1)));
  }
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Tight box of the item's painted pixels on the full-size canvas (text and captions: glyphs / box; shapes likewise), canvas pixels.
std::optional<QRect> paintedBox(const TimelineDoc& doc, const Item& it, Frame f) {
  const auto layer = render::rasterizeItem(doc, it, f, QSize(static_cast<int>(doc.project.width), static_cast<int>(doc.project.height)));
  if (!layer || layer->image.isNull()) return std::nullopt;
  const QImage& im = layer->image;
  int x0 = im.width(), y0 = im.height(), x1 = -1, y1 = -1;
  for (int y = 0; y < im.height(); ++y) {
    const uchar* row = im.constScanLine(y);
    for (int x = 0; x < im.width(); ++x)
      if (row[x * 4 + 3] > 8) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
      }
  }
  if (x1 < x0) return std::nullopt;
  return QRect(layer->origin.x() + x0, layer->origin.y() + y0, x1 - x0 + 1, y1 - y0 + 1);
}

// ---------------------------------------------------------------- text_legibility

void checkText(CallContext& c, const TimelineDoc& doc, Frame rs, Frame re, Out& out) {
  const double fps = fpsOf(doc);
  const double canvasW = static_cast<double>(doc.project.width), canvasH = static_cast<double>(doc.project.height);
  std::vector<const Item*> texts;
  for (const Item& it : doc.items)
    if (isTextItem(it) && !trackHidden(doc, it.trackId) && inRange(it, rs, re)) texts.push_back(&it);
  QJsonArray measuredItems;
  if (texts.empty()) {
    out.details.insert(q("textLegibility"), QJsonObject{{q("items"), measuredItems}});
    return;
  }

  // backgrounds: the doc without any text item, at <= 540 px wide, once per sampled frame
  const int bgW = static_cast<int>(std::min<std::int64_t>(540, doc.project.width));
  const int bgH = std::max(8, static_cast<int>(std::lround(bgW * canvasH / canvasW)));
  TimelineDoc bgDoc = scaledDoc(doc, bgW, bgH);
  std::erase_if(bgDoc.items, [](const Item& it) { return isTextItem(it); });
  std::set<Frame> want;
  for (const Item* it : texts)
    for (Frame f : samplesOf(*it, rs, re, 3)) want.insert(f);
  std::map<Frame, QImage> bg;
  const Result rr = renderFrames(c, bgDoc, std::vector<Frame>(want.begin(), want.end()), [&](Frame f, const QImage& im) { bg[f] = im; });
  const bool haveBg = rr.ok;
  if (!haveBg) out.skipped << QStringLiteral("text_legibility contrast: %1").arg(rr.error.message);

  const double k = bgW / canvasW;
  for (const Item* it : texts) {
    const TextStyle& st = *styleOf(*it);
    const QColor fill = parseCss(st.color, Qt::white);
    const double textLum = colourLum(fill);
    const double scale = std::abs(it->transform.scale * it->transform.scaleY);
    const double fontFrac = st.fontSize * scale / canvasH;
    const bool large = fontFrac >= 0.05;
    const double need = large ? 3.0 : 4.5;
    const double startSec = std::max(it->startFrame, rs) / fps, endSec = std::min(it->startFrame + it->durationFrames, re) / fps;
    const QString words = textOf(*it);
    const int nWords = static_cast<int>(words.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());

    // size (mobile) and on-screen time
    if (fontFrac < 0.025)
      out.warnings.append(entry(q("text_legibility"), q("text_too_small"), startSec, endSec, QJsonObject{{q("heightFraction"), r3(fontFrac)}, {q("fontSizePx"), r3(st.fontSize * scale)}},
                                QJsonObject{{q("minHeightFraction"), 0.025}},
                                QJsonObject{{q("suggest"), q("Raise the font size.")}, {q("options"), QJsonArray{QJsonObject{{q("type"), q("font_size")}, {q("minPx"), r3(0.025 * canvasH / std::max(scale, 1e-6))}}}}},
                                QStringLiteral("Text is %1 % of the frame height, under 2.5 %: too small on a phone.").arg(fontFrac * 100, 0, 'f', 1), it->id));
    if (it->type() == ItemType::Text) {
      const double sec = static_cast<double>(it->durationFrames) / fps, needSec = std::max(1.5, 0.3 * nWords);
      if (sec + 1e-9 < needSec)
        out.warnings.append(entry(q("text_legibility"), q("text_duration"), it->startFrame / fps, (it->startFrame + it->durationFrames) / fps,
                                  QJsonObject{{q("onScreenSec"), r3(sec)}, {q("words"), nWords}}, QJsonObject{{q("minSec"), r3(needSec)}, {q("secPerWord"), 0.3}, {q("floorSec"), 1.5}},
                                  QJsonObject{{q("suggest"), q("Lengthen the item.")}, {q("options"), QJsonArray{QJsonObject{{q("type"), q("duration")}, {q("durationFrames"), static_cast<double>(std::ceil(needSec * fps))}}}}},
                                  QStringLiteral("Text is on screen %1 s for %2 word(s); needs at least %3 s.").arg(sec, 0, 'f', 2).arg(nWords).arg(needSec, 0, 'f', 2), it->id));
    }

    // contrast
    const bool box = st.backgroundColor && parseCss(*st.backgroundColor, Qt::transparent).alphaF() >= 0.5;
    const bool stroked = st.strokeColor && st.strokeWidth >= std::max(1.0, 0.04 * st.fontSize);
    double worstRatio = 1e9, worstP10 = 0, worstP90 = 0, worstR10 = 0, worstR90 = 0;
    double strokeRatio = 0;
    int sampled = 0;
    if (stroked) strokeRatio = contrastRatio(textLum, colourLum(parseCss(*st.strokeColor, Qt::black)));
    if (box) {
      const double bl = colourLum(parseCss(*st.backgroundColor, Qt::black));
      worstP10 = worstP90 = bl;
      worstRatio = worstR10 = worstR90 = contrastRatio(textLum, bl);
      sampled = 1;
    } else if (haveBg) {
      for (Frame f : samplesOf(*it, rs, re, 3)) {
        const auto pb = paintedBox(doc, *it, f);
        const auto bi = bg.find(f);
        if (!pb || bi == bg.end()) continue;
        const QImage& im = bi->second;
        const QRect r = QRect(static_cast<int>(pb->x() * k), static_cast<int>(pb->y() * k), std::max(1, static_cast<int>(std::ceil(pb->width() * k))), std::max(1, static_cast<int>(std::ceil(pb->height() * k)))).intersected(im.rect());
        if (r.isEmpty()) continue;
        std::vector<float> lum;
        const auto& lut = srgbLut();
        for (int y = r.top(); y <= r.bottom(); ++y) {
          const uchar* row = im.constScanLine(y);
          for (int x = r.left(); x <= r.right(); ++x) lum.push_back(static_cast<float>(relLuminance(lut[row[x * 4]], lut[row[x * 4 + 1]], lut[row[x * 4 + 2]])));
        }
        const double p10 = percentile(lum, 0.10), p90 = percentile(lum, 0.90);
        const double r10 = contrastRatio(textLum, p10), r90 = contrastRatio(textLum, p90), worst = std::min(r10, r90);
        if (worst < worstRatio) {
          worstRatio = worst;
          worstP10 = p10;
          worstP90 = p90;
          worstR10 = r10;
          worstR90 = r90;
        }
        ++sampled;
      }
    }
    QJsonObject item{{q("itemId"), it->id}, {q("heightFraction"), r3(fontFrac)}, {q("large"), large}, {q("words"), nWords}};
    if (sampled > 0) {
      const double effective = stroked ? std::max(worstRatio, strokeRatio) : worstRatio;
      item.insert(q("textLuminance"), r3(textLum));
      item.insert(q("backgroundP10"), r3(worstP10));
      item.insert(q("backgroundP90"), r3(worstP90));
      item.insert(q("ratioP10"), r3(worstR10));
      item.insert(q("ratioP90"), r3(worstR90));
      item.insert(q("ratio"), r3(effective));
      item.insert(q("source"), box ? q("text box colour") : (stroked ? q("background behind the glyph box (frames rendered without text) or the stroke") : q("background behind the glyph box (frames rendered without text)")));
      if (effective < need) {
        const double midBg = (worstP10 + worstP90) / 2;
        const QString flip = contrastRatio(0.0, midBg) >= contrastRatio(1.0, midBg) ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
        const QString strokeCol = colourLum(fill) > 0.5 ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
        QJsonObject fix{{q("suggest"), q("Add a stroke or a box, or flip the text colour.")},
                        {q("options"), QJsonArray{QJsonObject{{q("type"), q("stroke")}, {q("strokeColor"), strokeCol}, {q("strokeWidth"), r3(std::max(2.0, st.fontSize * 0.08))}},
                                                  QJsonObject{{q("type"), q("box")}, {q("backgroundColor"), strokeCol == QLatin1String("#000000") ? q("rgba(0,0,0,0.6)") : q("rgba(255,255,255,0.7)")}, {q("padding"), r3(st.fontSize * 0.3)}},
                                                  QJsonObject{{q("type"), q("colorFlip")}, {q("color"), flip}}}}};
        out.violations.append(entry(q("text_legibility"), q("text_contrast"), startSec, endSec,
                                    QJsonObject{{q("ratio"), r3(effective)}, {q("ratioP10"), r3(worstR10)}, {q("ratioP90"), r3(worstR90)}, {q("textLuminance"), r3(textLum)}, {q("backgroundP10"), r3(worstP10)}, {q("backgroundP90"), r3(worstP90)}},
                                    QJsonObject{{q("minRatio"), need}, {q("large"), large}, {q("largeMinHeightFraction"), 0.05}}, fix,
                                    QStringLiteral("Contrast %1:1 between the text and its background is under %2:1 for %3 text.").arg(effective, 0, 'f', 2).arg(need, 0, 'f', 1).arg(large ? q("large") : q("small")), it->id));
      }
    }
    measuredItems.append(item);
  }
  out.details.insert(q("textLegibility"), QJsonObject{{q("items"), measuredItems}});
}

// ---------------------------------------------------------------- safe_zones

QJsonObject zonesJson() {
  QFile f(QStringLiteral(":/sf/style/safe_zones.json"));
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QJsonDocument::fromJson(f.readAll()).object();
}

bool taggedImportant(const Item& it) { return it.labels.name && it.labels.name->contains(QStringLiteral("important"), Qt::CaseInsensitive); }

std::optional<Rect> normalisedBounds(const TimelineDoc& doc, const editor::Project& proj, const Item& it, Frame rs, Frame re) {
  const double cw = static_cast<double>(doc.project.width), ch = static_cast<double>(doc.project.height);
  Rect u;
  if (it.type() == ItemType::Video || it.type() == ItemType::Image) {
    if (!it.assetId) return std::nullopt;
    const Asset* as = proj.asset(*it.assetId);
    if (!as || as->width <= 0 || as->height <= 0) return std::nullopt;
    const render::Placement pl = render::placeMedia(it, std::max(it.startFrame, rs), QSizeF(as->width, as->height), QSize(static_cast<int>(cw), static_cast<int>(ch)));
    const double w = pl.size.width() * std::abs(pl.scaleX), h = pl.size.height() * std::abs(pl.scaleY);
    return Rect{(pl.center.x() - w / 2) / cw, (pl.center.y() - h / 2) / ch, w / cw, h / ch};
  }
  for (Frame f : samplesOf(it, rs, re, 3)) {
    const auto b = paintedBox(doc, it, f);
    if (b) u = unite(u, Rect{b->x() / cw, b->y() / ch, b->width() / cw, b->height() / ch});
  }
  if (u.area() <= 0) return std::nullopt;
  return u;
}

// Smallest axis-aligned move (canvas px for transform.x / y) that takes `box` out of `zone`.
QJsonObject moveOut(const Rect& box, const Rect& zone, double cw, double ch) {
  const double left = -(box.x + box.w - zone.x), right = zone.x + zone.w - box.x, up = -(box.y + box.h - zone.y), down = zone.y + zone.h - box.y;
  struct Opt {
    double d;
    bool horizontal;
  };
  const Opt opts[4] = {{left, true}, {right, true}, {up, false}, {down, false}};
  const Opt* best = &opts[0];
  for (const Opt& o : opts)
    if (std::abs(o.d) < std::abs(best->d)) best = &o;
  return QJsonObject{{q("type"), q("move")}, {q("transformDxPx"), r3(best->horizontal ? best->d * cw : 0.0)}, {q("transformDyPx"), r3(best->horizontal ? 0.0 : best->d * ch)}};
}

void checkSafeZones(const TimelineDoc& doc, const editor::Project& proj, Frame rs, Frame re, const QString& platform, double minOverlap, Out& out) {
  const double fps = fpsOf(doc), cw = static_cast<double>(doc.project.width), ch = static_cast<double>(doc.project.height);
  const QJsonObject zj = zonesJson();
  const QJsonObject plat = zj.value(q("platforms")).toObject().value(platform).toObject();
  QJsonArray checked;
  for (const Item& it : doc.items) {
    if (trackHidden(doc, it.trackId) || !inRange(it, rs, re)) continue;
    if (!isTextItem(it) && !taggedImportant(it)) continue;
    const auto nb = normalisedBounds(doc, proj, it, rs, re);
    if (!nb) continue;
    const Rect box = *nb;
    const double startSec = std::max(it.startFrame, rs) / fps, endSec = std::min(it.startFrame + it.durationFrames, re) / fps;
    const QJsonObject bj{{q("x"), r3(box.x)}, {q("y"), r3(box.y)}, {q("w"), r3(box.w)}, {q("h"), r3(box.h)}};
    checked.append(QJsonObject{{q("itemId"), it.id}, {q("bounds"), bj}});
    for (const QJsonValue& zv : plat.value(q("zones")).toArray()) {
      const QJsonObject z = zv.toObject();
      const QJsonObject rj = z.value(q("rect")).toObject();
      const Rect zr{rj.value(q("x")).toDouble(), rj.value(q("y")).toDouble(), rj.value(q("w")).toDouble(), rj.value(q("h")).toDouble()};
      const double frac = intersectArea(box, zr) / box.area();
      if (frac < minOverlap) continue;
      out.add(z.value(q("severity")).toString(),
              entry(q("safe_zones"), q("safe_zone_overlap"), startSec, endSec, QJsonObject{{q("overlapFraction"), r3(frac)}, {q("zone"), z.value(q("name")).toString()}, {q("bounds"), bj}, {q("platform"), platform}},
                    QJsonObject{{q("maxOverlapFraction"), minOverlap}, {q("zoneRect"), rj}},
                    QJsonObject{{q("suggest"), QStringLiteral("Move the item out of the %1 zone.").arg(z.value(q("name")).toString())}, {q("options"), QJsonArray{moveOut(box, zr, cw, ch)}}},
                    QStringLiteral("%1 % of the item sits in the %2 %3 UI zone.").arg(frac * 100, 0, 'f', 0).arg(platform, z.value(q("name")).toString()), it.id));
    }
    for (const QJsonValue& mv : plat.value(q("margins")).toArray()) {
      const QJsonObject m = mv.toObject();
      const double in = m.value(q("inset")).toDouble();
      const Rect safe{in, in, 1 - 2 * in, 1 - 2 * in};
      const double outside = 1.0 - intersectArea(box, safe) / box.area();
      if (outside < minOverlap) continue;
      const double dx = std::max(safe.x - box.x, 0.0) - std::max(box.x + box.w - (safe.x + safe.w), 0.0);
      const double dy = std::max(safe.y - box.y, 0.0) - std::max(box.y + box.h - (safe.y + safe.h), 0.0);
      out.add(m.value(q("severity")).toString(),
              entry(q("safe_zones"), q("outside_safe_area"), startSec, endSec, QJsonObject{{q("outsideFraction"), r3(outside)}, {q("margin"), m.value(q("name")).toString()}, {q("bounds"), bj}, {q("platform"), platform}},
                    QJsonObject{{q("maxOutsideFraction"), minOverlap}, {q("inset"), in}},
                    QJsonObject{{q("suggest"), QStringLiteral("Move the item inside the %1 area.").arg(m.value(q("name")).toString())},
                                {q("options"), QJsonArray{QJsonObject{{q("type"), q("move")}, {q("transformDxPx"), r3(dx * cw)}, {q("transformDyPx"), r3(dy * ch)}}}}},
                    QStringLiteral("%1 % of the item lies outside the %2 area.").arg(outside * 100, 0, 'f', 0).arg(m.value(q("name")).toString()), it.id));
    }
  }
  out.details.insert(q("safeZones"), QJsonObject{{q("platform"), platform}, {q("source"), zj.value(q("source")).toString()}, {q("itemsChecked"), checked}});
}

// ---------------------------------------------------------------- command

Result safetyCmd(CallContext& c, const QJsonObject& a) {
  const editor::Project& p = *c.session->project;
  const TimelineDoc& doc = p.doc();
  const double fps = fpsOf(doc);
  const Frame total = std::max<Frame>(docDurationFrames(doc), 0);
  Frame rs = 0, re = total;
  if (a.contains(q("range"))) {
    const QJsonObject r = a.value(q("range")).toObject();
    if (r.contains(q("start"))) rs = static_cast<Frame>(std::llround(r.value(q("start")).toDouble() * fps));
    if (r.contains(q("end"))) re = static_cast<Frame>(std::llround(r.value(q("end")).toDouble() * fps));
  }
  rs = std::clamp<Frame>(rs, 0, total);
  re = std::clamp<Frame>(re, rs, total);
  if (re <= rs) return invalid(QStringLiteral("The range is empty (the timeline runs %1 frames).").arg(total));

  QStringList checks{q("photosensitive"), q("text_legibility"), q("safe_zones")};
  if (a.contains(q("checks"))) {
    checks.clear();
    for (const QJsonValue& v : a.value(q("checks")).toArray()) {
      const QString n = v.toString();
      if (n != QLatin1String("photosensitive") && n != QLatin1String("text_legibility") && n != QLatin1String("safe_zones")) return invalid(QStringLiteral("Unknown check \"%1\": photosensitive, text_legibility or safe_zones.").arg(n));
      checks << n;
    }
  }
  const QJsonObject zj = zonesJson();
  const QJsonObject platforms = zj.value(q("platforms")).toObject();
  QString platform = a.value(q("platform")).toString();
  if (platform.isEmpty()) platform = doc.project.height > doc.project.width ? q("tiktok") : q("youtube");
  if (!platforms.contains(platform)) return invalid(QStringLiteral("Unknown platform \"%1\": %2.").arg(platform, platforms.keys().join(QStringLiteral(", "))));

  Out out;
  if (checks.contains(q("photosensitive")))
    checkPhotosensitive(c, doc, rs, re, a.value(q("analysisWidth")).toInt(160), std::clamp(a.value(q("maxFrames")).toInt(1800), 2, 20000), out);
  if (checks.contains(q("text_legibility"))) checkText(c, doc, rs, re, out);
  if (checks.contains(q("safe_zones"))) {
    const double asp = static_cast<double>(doc.project.width) / static_cast<double>(std::max<std::int64_t>(1, doc.project.height));
    const QStringList parts = platforms.value(platform).toObject().value(q("aspect")).toString().split(u':');
    const double want = parts.size() == 2 && parts[1].toDouble() > 0 ? parts[0].toDouble() / parts[1].toDouble() : 0;
    if (want > 0 && std::abs(asp / want - 1.0) > 0.05)
      out.info.append(entry(q("safe_zones"), q("platform_aspect"), rs / fps, re / fps, QJsonObject{{q("canvasAspect"), r3(asp)}}, QJsonObject{{q("platformAspect"), r3(want)}}, {},
                            QStringLiteral("The canvas is not %1; the normalised zones are applied anyway.").arg(platforms.value(platform).toObject().value(q("aspect")).toString())));
    checkSafeZones(doc, p, rs, re, platform, a.value(q("minOverlapFraction")).toDouble(0.02), out);
  }

  QJsonObject o{{q("pass"), out.violations.isEmpty()},
                {q("violations"), out.violations},
                {q("warnings"), out.warnings},
                {q("info"), out.info},
                {q("summary"), QJsonObject{{q("violations"), static_cast<double>(out.violations.size())}, {q("warnings"), static_cast<double>(out.warnings.size())}, {q("info"), static_cast<double>(out.info.size())},
                                           {q("checks"), QJsonArray::fromStringList(checks)}, {q("skipped"), QJsonArray::fromStringList(out.skipped)}}},
                {q("platform"), platform},
                {q("range"), QJsonObject{{q("startSec"), r3(rs / fps)}, {q("endSec"), r3(re / fps)}, {q("startFrame"), static_cast<double>(rs)}, {q("endFrame"), static_cast<double>(re)}}}};
  for (auto it = out.details.begin(); it != out.details.end(); ++it) o.insert(it.key(), it.value());
  return Result::success(o);
}

} // namespace

void registerSafetyCommands(Engine& e) {
  e.add(spec("safety.check", "", "editor", false,
             QStringLiteral("Deterministic delivery-safety checks measured from the render and the document. Returns {pass, violations[], warnings[], info[], summary, ...details}; every entry is {check, kind, startSec, endSec, itemId?, measured{}, threshold{}, fix?} "
                            "(fix is data only: what to change, never applied). `range` {start, end} in seconds (default the whole timeline); `checks` any of photosensitive, text_legibility, safe_zones (default all). "
                            "photosensitive (WCAG 2.3.1 / 2.3.2, ITU-R BT.1702): the range is rendered at project fps and `analysisWidth` px wide (default 160, `maxFrames` default 1800); per frame pair, pixels whose linear luminance moves >= 10 % "
                            "with the darker state < 0.80 form a change when they cover >= 11.1 % of the frame (WCAG 341x256 of 1024x768; a whole-frame area fraction, not a region search); more than 3 alternating-change flashes in any 1 s window "
                            "is a violation, general (photosensitive_general_flash) and red ((R-G-B)*320 moves > 20 with a saturated-red state, photosensitive_red_flash) separately; flashesPerSecondWindow and worstWindow are in `photosensitive`. "
                            "A regular-stripe pattern across the frame is a best-effort warning (photosensitive_pattern). "
                            "text_legibility: for every text / caption item in range, WCAG contrast between the text colour and the p10 / p90 luminance of the background behind the glyph box (the same frames rendered without any text; the box colour when the "
                            "item has one; a visible stroke counts as the background): under 4.5:1 (3:1 when the font is >= 5 % of the frame height) is a violation (text_contrast); text under 2.5 % of the height (text_too_small) and a text item on screen less than "
                            "max(1.5 s, 0.3 s per word) (text_duration) are warnings. safe_zones: text / caption items and items whose label name contains \"important\" are tested against the platform's UI zones and safe margins "
                            "(src/engine/styles/safe_zones.json: tiktok, instagram_reels, youtube_shorts, youtube, broadcast_title_safe; approximate, community-measured); `platform` defaults to tiktok on a portrait canvas, youtube otherwise; "
                            "an overlap of at least `minOverlapFraction` (0.02) of the item is reported at the zone's severity."),
             js::obj({{q("range"), js::obj({{q("start"), js::number(QStringLiteral("seconds"), 0)}, {q("end"), js::number(QStringLiteral("seconds"), 0)}})},
                      {q("platform"), js::choice({q("tiktok"), q("instagram_reels"), q("youtube_shorts"), q("youtube"), q("broadcast_title_safe")})},
                      {q("checks"), js::array(js::choice({q("photosensitive"), q("text_legibility"), q("safe_zones")}))},
                      {q("analysisWidth"), js::integer(QStringLiteral("Width in px of the flash analysis render, default 160"), 16, 1280)},
                      {q("maxFrames"), js::integer(QStringLiteral("Most frames the flash analysis renders, default 1800"), 2, 20000)},
                      {q("minOverlapFraction"), js::number(QString(), 0, 1)}}),
             safetyCmd));
}

} // namespace sf::engine
