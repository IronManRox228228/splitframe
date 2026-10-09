#include "render/layer_painter.h"

#include "render/layout.h"

#include <QCryptographicHash>
#include <QFont>
#include <QFontMetricsF>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace sf::render {

namespace {

// CSS colour strings as the schema stores them: #rgb, #rrggbb, #rrggbbaa (alpha LAST, unlike QColor),
// rgb()/rgba(), or a keyword.
QColor cssColor(const QString& text, const QColor& fallback = Qt::transparent) {
  const QString s = text.trimmed();
  if (s.isEmpty()) return fallback;
  if (s.startsWith(u'#')) {
    const QString h = s.mid(1);
    bool ok = false;
    if (h.size() == 8) {
      const uint v = h.toUInt(&ok, 16);
      if (ok) return QColor(static_cast<int>((v >> 24) & 255), static_cast<int>((v >> 16) & 255), static_cast<int>((v >> 8) & 255), static_cast<int>(v & 255));
    } else if (h.size() == 4) {
      const uint v = h.toUInt(&ok, 16);
      if (ok) return QColor(static_cast<int>(((v >> 12) & 15) * 17), static_cast<int>(((v >> 8) & 15) * 17), static_cast<int>(((v >> 4) & 15) * 17), static_cast<int>((v & 15) * 17));
    }
    const QColor c(s);
    return c.isValid() ? c : fallback;
  }
  static const QRegularExpression rgb(QStringLiteral(R"(^rgba?\(\s*([\d.]+)[\s,]+([\d.]+)[\s,]+([\d.]+)(?:[\s,/]+([\d.]+%?))?\s*\)$)"));
  if (const auto m = rgb.match(s); m.hasMatch()) {
    double a = 1;
    if (!m.captured(4).isEmpty()) a = m.captured(4).endsWith(u'%') ? m.captured(4).chopped(1).toDouble() / 100 : m.captured(4).toDouble();
    return QColor(static_cast<int>(std::lround(m.captured(1).toDouble())), static_cast<int>(std::lround(m.captured(2).toDouble())),
                  static_cast<int>(std::lround(m.captured(3).toDouble())), static_cast<int>(std::lround(std::clamp(a, 0.0, 1.0) * 255)));
  }
  const QColor c(s);
  return c.isValid() ? c : fallback;
}

QFont fontFor(const TextStyle& s) {
  QFont f;
  // the reference's CSS stack: the item's family, then Geist, then the system UI font
  f.setFamilies({s.fontFamily, QStringLiteral("Geist"), QStringLiteral("Segoe UI"), QStringLiteral("Arial")});
  f.setPixelSize(std::max(1, static_cast<int>(std::lround(s.fontSize))));
  f.setWeight(static_cast<QFont::Weight>(std::clamp<std::int64_t>(s.fontWeight, 1, 1000)));
  f.setHintingPreference(QFont::PreferNoHinting);
  return f;
}

QFont plainFont(int px, int weight = 400) {
  QFont f;
  f.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Arial")});
  f.setPixelSize(px);
  f.setWeight(static_cast<QFont::Weight>(weight));
  return f;
}

QString hashOf(const QByteArray& data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex()); }

QString xfKey(const QTransform& t, QSize canvas) {
  return QStringLiteral("%1x%2|%3,%4,%5,%6,%7,%8").arg(canvas.width()).arg(canvas.height()).arg(t.m11(), 0, 'g', 9).arg(t.m12(), 0, 'g', 9)
      .arg(t.m21(), 0, 'g', 9).arg(t.m22(), 0, 'g', 9).arg(t.dx(), 0, 'g', 9).arg(t.dy(), 0, 'g', 9);
}

QTransform itemTransform(double cx, double cy, double rotation, double sx, double sy) {
  QTransform t;
  t.translate(cx, cy);
  t.rotate(rotation);
  t.scale(sx, sy);
  return t;
}

// Paints with `draw` (in the item's local space) into an image just big enough for what lands on the canvas.
template <class F>
std::optional<RasterLayer> paintLayer(const QTransform& xf, QRectF local, QSize canvas, const QString& key, F&& draw) {
  const QRect bounds = xf.mapRect(local).toAlignedRect().adjusted(-1, -1, 1, 1).intersected(QRect(QPoint(0, 0), canvas));
  if (bounds.isEmpty()) return std::nullopt;
  RasterLayer out;
  out.image = QImage(bounds.size(), QImage::Format_RGBA8888_Premultiplied);
  out.image.fill(Qt::transparent);
  out.origin = bounds.topLeft();
  out.key = key;
  QPainter p(&out.image);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  p.translate(-bounds.topLeft());
  p.setTransform(xf, true);
  draw(p);
  p.end();
  return out;
}

void roundedRect(QPainterPath& path, double x, double y, double w, double h, double r) {
  const double radius = std::max(0.0, std::min({r, w / 2, h / 2}));
  if (radius <= 0) path.addRect(x, y, w, h);
  else path.addRoundedRect(QRectF(x, y, w, h), radius, radius);
}

// canvas strokeText + fillText: stroke first (centered on the outline, mitre joins), fill on top
void strokeThenFill(QPainter& p, const QPainterPath& path, double strokeWidth, const QColor& stroke, const QColor& fill) {
  if (strokeWidth > 0 && stroke.alpha() > 0) {
    QPen pen(stroke, strokeWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    pen.setMiterLimit(10);
    p.strokePath(path, pen);
  }
  p.fillPath(path, fill);
}

// canvas textBaseline "middle": the middle of the em box sits on y
double baselineFor(const QFontMetricsF& fm, double y) { return y + (fm.ascent() - fm.descent()) / 2; }

std::optional<RasterLayer> rasterText(const Item& item, const TextProps& props, Frame frame, QSize canvas) {
  const TextStyle& style = props.style;
  const double scale = resolveProperty(item, QStringLiteral("transform.scale"), frame);
  const QTransform xf = itemTransform(canvas.width() / 2.0 + resolveProperty(item, QStringLiteral("transform.x"), frame),
                                      canvas.height() / 2.0 + resolveProperty(item, QStringLiteral("transform.y"), frame),
                                      resolveProperty(item, QStringLiteral("transform.rotation"), frame), scale * item.transform.scaleX,
                                      scale * item.transform.scaleY);
  QStringList lines = props.text.split(u'\n');
  if (style.uppercase) {
    for (QString& l : lines) l = l.toUpper();
  }
  const QFont font = fontFor(style);
  const QFontMetricsF fm(font);
  const double lineHeight = style.fontSize * style.lineHeight;
  const auto n = static_cast<double>(lines.size());
  double widest = 0;
  for (const QString& l : std::as_const(lines)) widest = std::max(widest, fm.horizontalAdvance(l));

  QRectF local(-widest / 2, -n * lineHeight / 2, widest, n * lineHeight);
  if (style.align == TextAlign::Left) local = QRectF(0, local.y(), widest, local.height());
  if (style.align == TextAlign::Right) local = QRectF(-widest, local.y(), widest, local.height());
  const double pad = style.backgroundColor ? style.padding : 0;
  if (style.backgroundColor) local = local.united(QRectF(-widest / 2 - pad, -n * lineHeight / 2 - pad * 0.5, widest + pad * 2, n * lineHeight + pad));
  local.adjust(-style.strokeWidth - style.fontSize * 0.3, -style.strokeWidth - style.fontSize * 0.3, style.strokeWidth + style.fontSize * 0.3,
               style.strokeWidth + style.fontSize * 0.3);

  const QString key = QStringLiteral("text|%1|%2|%3").arg(item.id, hashOf(QJsonDocument(toJson(props.style)).toJson(QJsonDocument::Compact) + props.text.toUtf8()), xfKey(xf, canvas));
  return paintLayer(xf, local, canvas, key, [&](QPainter& p) {
    if (style.backgroundColor) {
      QPainterPath bg;
      roundedRect(bg, -widest / 2 - pad, -n * lineHeight / 2 - pad * 0.5, widest + pad * 2, n * lineHeight + pad, style.borderRadius);
      p.fillPath(bg, cssColor(*style.backgroundColor));
    }
    const QColor fill = cssColor(style.color, Qt::white);
    const QColor stroke = style.strokeColor ? cssColor(*style.strokeColor) : QColor(Qt::transparent);
    for (qsizetype i = 0; i < lines.size(); ++i) {
      const double y = (static_cast<double>(i) - (n - 1) / 2) * lineHeight;
      const double w = fm.horizontalAdvance(lines[i]);
      const double x = style.align == TextAlign::Center ? -w / 2 : (style.align == TextAlign::Right ? -w : 0);
      QPainterPath path;
      path.addText(QPointF(x, baselineFor(fm, y)), font, lines[i]);
      strokeThenFill(p, path, style.strokeWidth, stroke, fill);
    }
  });
}

std::optional<RasterLayer> rasterCaption(const TimelineDoc& doc, const Item& item, const CaptionProps& props, Frame frame, QSize canvas) {
  const CaptionStyle& style = props.style;
  const Ms timeMs = captionClockMs(frame, item.startFrame, static_cast<double>(doc.project.fps));
  const auto shown = captionCardAt(props.words, timeMs, props.maxWordsPerCard);
  if (!shown) return std::nullopt;
  const double scale = resolveProperty(item, QStringLiteral("transform.scale"), frame);
  // the reference reads the static transform.x/y here (not keyframes) and ignores scaleX/scaleY
  const QTransform xf = itemTransform(canvas.width() / 2.0 + item.transform.x, canvas.height() * style.placementY + item.transform.y,
                                      resolveProperty(item, QStringLiteral("transform.rotation"), frame), scale, scale);
  const QFont font = fontFor(style);
  const QFontMetricsF fm(font);

  struct W {
    QString text;
    bool active;
  };
  std::vector<std::vector<W>> lines(1);
  std::int64_t chars = 0;
  for (const TranscriptWord* word : shown->words) {
    const bool active = timeMs >= word->startMs && timeMs < word->endMs;
    const auto len = static_cast<std::int64_t>(word->w.size()) + 1;
    if (chars + len > style.maxCharsPerLine && !lines.back().empty()) {
      lines.emplace_back();
      chars = 0;
    }
    lines.back().push_back({word->w, active});
    chars += len;
  }
  const double lineHeight = style.fontSize * style.lineHeight * 1.15;
  const double spaceW = fm.horizontalAdvance(QLatin1Char(' '));
  const auto n = static_cast<double>(lines.size());
  double widest = 0;
  for (const auto& line : lines) {
    double total = spaceW * static_cast<double>(line.size() - 1);
    for (const W& w : line) total += fm.horizontalAdvance(w.text);
    widest = std::max(widest, total);
  }
  QRectF local(-widest / 2 - 12, -n * lineHeight / 2 - style.fontSize * 0.3, widest + 24, n * lineHeight + style.fontSize * 0.6);
  local.adjust(-style.strokeWidth, -style.strokeWidth, style.strokeWidth, style.strokeWidth);

  QString content;
  for (const auto& line : lines) {
    for (const W& w : line) content += w.text + (w.active ? u'*' : u' ');
    content += u'\n';
  }
  const QString key = QStringLiteral("caption|%1|%2|%3").arg(item.id, hashOf(QJsonDocument(toJson(style)).toJson(QJsonDocument::Compact) + content.toUtf8()), xfKey(xf, canvas));
  return paintLayer(xf, local, canvas, key, [&](QPainter& p) {
    const QColor base = cssColor(style.color, Qt::white);
    const QColor hi = cssColor(style.highlightColor, QColor(0xfb, 0xbf, 0x24));
    const QColor stroke = cssColor(style.strokeColor.value_or(QStringLiteral("#000000")));
    for (qsizetype li = 0; li < static_cast<qsizetype>(lines.size()); ++li) {
      const auto& line = lines[static_cast<size_t>(li)];
      const double y = (static_cast<double>(li) - (n - 1) / 2) * lineHeight;
      double total = spaceW * static_cast<double>(line.size() - 1);
      for (const W& w : line) total += fm.horizontalAdvance(w.text);
      double x = -total / 2;
      for (const W& w : line) {
        const double ww = fm.horizontalAdvance(w.text);
        if (style.highlight == CaptionHighlight::WordBg && w.active) {
          QPainterPath bg;
          roundedRect(bg, x - 4, y - style.fontSize * 0.6, ww + 8, style.fontSize * 1.2, 6);
          p.fillPath(bg, hi);
        }
        QPainterPath path;
        path.addText(QPointF(x, baselineFor(fm, y)), font, w.text);
        const bool lit = style.highlight == CaptionHighlight::ActiveWord && w.active;
        strokeThenFill(p, path, style.strokeWidth, stroke, lit ? hi : base);
        x += ww + spaceW;
      }
    }
  });
}

std::optional<RasterLayer> rasterShape(const Item& item, const ShapeProps& props, Frame frame, QSize canvas) {
  const double scale = resolveProperty(item, QStringLiteral("transform.scale"), frame);
  const QTransform xf = itemTransform(canvas.width() / 2.0 + resolveProperty(item, QStringLiteral("transform.x"), frame),
                                      canvas.height() / 2.0 + resolveProperty(item, QStringLiteral("transform.y"), frame),
                                      resolveProperty(item, QStringLiteral("transform.rotation"), frame), scale * item.transform.scaleX,
                                      scale * item.transform.scaleY);
  const double w = props.width, h = props.height;
  const QRectF local(-w / 2 - props.strokeWidth, -h / 2 - props.strokeWidth, w + 2 * props.strokeWidth, h + 2 * props.strokeWidth);
  const QString key = QStringLiteral("shape|%1|%2|%3").arg(item.id, hashOf(QJsonDocument(toJson(ItemProps(props))).toJson(QJsonDocument::Compact)), xfKey(xf, canvas));
  return paintLayer(xf, local, canvas, key, [&](QPainter& p) {
    QPainterPath path;
    switch (props.shape) {
    case ShapeKind::Rect: roundedRect(path, -w / 2, -h / 2, w, h, props.radius); break;
    case ShapeKind::Ellipse: path.addEllipse(QRectF(-w / 2, -h / 2, w, h)); break;
    case ShapeKind::Triangle:
      path.moveTo(0, -h / 2);
      path.lineTo(w / 2, h / 2);
      path.lineTo(-w / 2, h / 2);
      path.closeSubpath();
      break;
    }
    p.fillPath(path, cssColor(props.fill));
    if (props.strokeWidth > 0 && props.stroke) {
      QPen pen(cssColor(*props.stroke), props.strokeWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
      pen.setMiterLimit(10);
      p.strokePath(path, pen);
    }
  });
}

std::optional<RasterLayer> rasterCard(const Item& item, Frame frame, QSize canvas, bool motion) {
  const double scale = resolveProperty(item, QStringLiteral("transform.scale"), frame);
  const QTransform xf = itemTransform(canvas.width() / 2.0 + resolveProperty(item, QStringLiteral("transform.x"), frame),
                                      canvas.height() / 2.0 + resolveProperty(item, QStringLiteral("transform.y"), frame),
                                      resolveProperty(item, QStringLiteral("transform.rotation"), frame), 1, 1);
  const QString name = item.labels.name.value_or(motion ? QStringLiteral("Motion graphic") : item.id);
  const double w = motion ? 480 * scale : canvas.width() * 0.6 * scale;
  const double h = motion ? 270 * scale : canvas.height() * 0.6 * scale;
  const QRectF box(-w / 2, -h / 2, w, h);
  const QString key = QStringLiteral("card|%1|%2|%3|%4|%5").arg(item.id, name).arg(motion).arg(scale, 0, 'g', 9).arg(xfKey(xf, canvas));
  return paintLayer(xf, box.adjusted(-4, -4, 4, 4), canvas, key, [&](QPainter& p) {
    if (motion) {
      QPainterPath path;
      roundedRect(path, box.x(), box.y(), w, h, 12);
      p.fillPath(path, QColor(59, 130, 246, 38));
      p.strokePath(path, QPen(QColor(0x3b, 0x82, 0xf6), 2));
      p.setFont(plainFont(24, 600));
      p.setPen(QColor(0x93, 0xc5, 0xfd));
      p.drawText(box, Qt::AlignCenter, QStringLiteral("▶ ") + name);
    } else {
      p.fillRect(box, QColor(0x1f, 0x29, 0x37));
      QPen pen(QColor(0x6b, 0x72, 0x80), 1, Qt::CustomDashLine);
      pen.setDashPattern({8, 6});
      p.setPen(pen);
      p.drawRect(box);
      p.setFont(plainFont(20));
      p.setPen(QColor(0x9c, 0xa3, 0xaf));
      p.drawText(box, Qt::AlignCenter, QStringLiteral("Media unavailable — ") + name);
    }
  });
}

} // namespace

std::optional<RasterLayer> rasterizeItem(const TimelineDoc& doc, const Item& item, Frame frame, QSize canvas) {
  if (const auto* t = std::get_if<TextProps>(&item.props)) return rasterText(item, *t, frame, canvas);
  if (const auto* c = std::get_if<CaptionProps>(&item.props)) return rasterCaption(doc, item, *c, frame, canvas);
  if (const auto* s = std::get_if<ShapeProps>(&item.props)) return rasterShape(item, *s, frame, canvas);
  if (std::holds_alternative<MotionGraphicProps>(item.props)) return rasterCard(item, frame, canvas, true);
  return std::nullopt;
}

std::optional<RasterLayer> rasterizeMissingMedia(const Item& item, Frame frame, QSize canvas) { return rasterCard(item, frame, canvas, false); }

} // namespace sf::render
