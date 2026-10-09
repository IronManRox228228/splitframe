#include "render/layout.h"

#include "core/timeline_doc.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace sf::render {

std::vector<const Item*> activeItems(const TimelineDoc& doc, Frame frame) {
  std::vector<const Item*> out;
  for (auto t = doc.tracks.rbegin(); t != doc.tracks.rend(); ++t) {
    if (t->hidden) continue;
    for (const Item* item : itemsOnTrack(doc, t->id)) {
      if (frame >= item->startFrame && frame < itemEnd(*item)) out.push_back(item);
    }
  }
  return out;
}

namespace {

double applyEasing(double t, Easing e) {
  switch (e) {
  case Easing::Hold: return 0;
  case Easing::EaseIn: return t * t;
  case Easing::EaseOut: return 1 - (1 - t) * (1 - t);
  case Easing::EaseInOut: return t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
  default: return t;
  }
}

} // namespace

double resolveKeyframes(const KeyframeList& kfs, Frame localFrame) {
  KeyframeList sorted = kfs;
  std::stable_sort(sorted.begin(), sorted.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
  const Keyframe& first = sorted.front();
  const Keyframe& last = sorted.back();
  if (localFrame <= first.frame) return first.value;
  if (localFrame >= last.frame) return last.value;
  for (std::size_t i = 0; i + 1 < sorted.size(); ++i) {
    const Keyframe& a = sorted[i];
    const Keyframe& b = sorted[i + 1];
    if (localFrame >= a.frame && localFrame <= b.frame) {
      const double span = static_cast<double>(std::max<Frame>(1, b.frame - a.frame));
      const double t = applyEasing(static_cast<double>(localFrame - a.frame) / span, b.easing);
      return a.value + (b.value - a.value) * t;
    }
  }
  return last.value;
}

double resolveProperty(const Item& item, const QString& path, Frame frame) {
  const Frame local = frame - item.startFrame;
  if (const auto it = item.keyframes.find(path); it != item.keyframes.end() && !it->second.empty()) {
    return resolveKeyframes(it->second, local);
  }
  const Transform& t = item.transform;
  if (path == QLatin1String("transform.x")) return t.x;
  if (path == QLatin1String("transform.y")) return t.y;
  if (path == QLatin1String("transform.scale")) return t.scale;
  if (path == QLatin1String("transform.scaleX")) return t.scaleX;
  if (path == QLatin1String("transform.scaleY")) return t.scaleY;
  if (path == QLatin1String("transform.rotation")) return t.rotation;
  if (path == QLatin1String("transform.opacity")) return t.opacity;
  if (path == QLatin1String("volume")) return item.volume;
  if (path == QLatin1String("speed")) return item.speed;
  return 0;
}

double opacityAt(const Item& item, Frame frame) {
  double alpha = resolveProperty(item, QStringLiteral("transform.opacity"), frame);
  const Frame local = frame - item.startFrame;
  Frame fadeIn = 0, fadeOut = 0;
  if (const auto* v = std::get_if<VideoProps>(&item.props)) {
    fadeIn = v->fadeInFrames;
    fadeOut = v->fadeOutFrames;
  } else if (const auto* a = std::get_if<AudioProps>(&item.props)) {
    fadeIn = a->fadeInFrames;
    fadeOut = a->fadeOutFrames;
  }
  if (fadeIn > 0 && local < fadeIn) alpha *= std::max(0.0, static_cast<double>(local) / static_cast<double>(fadeIn));
  const Frame fromEnd = item.durationFrames - local;
  if (fadeOut > 0 && fromEnd < fadeOut) alpha *= std::max(0.0, static_cast<double>(fromEnd) / static_cast<double>(fadeOut));
  return std::clamp(alpha, 0.0, 1.0);
}

Ms captionClockMs(Frame frame, Frame itemStart, double fps) {
  return jsRound(static_cast<double>(frame - itemStart) / fps * 1000.0);
}

std::optional<CaptionCard> captionCardAt(const std::vector<TranscriptWord>& words, Ms timeMs, std::int64_t maxWordsPerCard) {
  if (words.empty() || timeMs < words.front().startMs) return std::nullopt;
  const auto n = static_cast<qsizetype>(words.size());
  qsizetype active = -1;
  for (qsizetype i = 0; i < n; ++i) {
    const TranscriptWord& w = words[static_cast<size_t>(i)];
    if (timeMs >= w.startMs && timeMs < w.endMs) {
      active = i;
      break;
    }
  }
  qsizetype anchor = active;
  if (anchor == -1) {
    for (qsizetype i = n - 1; i >= 0; --i) {
      if (words[static_cast<size_t>(i)].startMs <= timeMs) {
        anchor = i;
        break;
      }
    }
  }
  const auto perCard = static_cast<qsizetype>(std::max<std::int64_t>(1, maxWordsPerCard));
  CaptionCard card;
  card.activeIdx = static_cast<int>(active);
  card.firstWord = anchor / perCard * perCard;
  for (qsizetype i = card.firstWord; i < std::min(card.firstWord + perCard, n); ++i) {
    card.words.push_back(&words[static_cast<size_t>(i)]);
  }
  if (card.words.empty()) return std::nullopt;
  return card;
}

QTransform Placement::unitToCanvas() const {
  QTransform t;
  t.translate(center.x(), center.y());
  t.rotate(rotationDeg);
  t.scale(scaleX * size.width(), scaleY * size.height());
  t.translate(-0.5, -0.5);
  return t;
}

Placement placeMedia(const Item& item, Frame frame, QSizeF content, QSize canvas) {
  Placement p;
  p.center = {canvas.width() / 2.0 + resolveProperty(item, QStringLiteral("transform.x"), frame),
              canvas.height() / 2.0 + resolveProperty(item, QStringLiteral("transform.y"), frame)};
  const double scale = resolveProperty(item, QStringLiteral("transform.scale"), frame);
  p.scaleX = scale * item.transform.scaleX;
  p.scaleY = scale * item.transform.scaleY;
  p.rotationDeg = resolveProperty(item, QStringLiteral("transform.rotation"), frame);
  if (content.width() > 0 && content.height() > 0) {
    const double fit = std::min(canvas.width() / content.width(), canvas.height() / content.height());
    p.size = {content.width() * fit, content.height() * fit};
  }
  return p;
}

namespace {

using M3 = std::array<std::array<double, 3>, 3>;

double num(const EffectParams& p, const char* key, double fallback) {
  const auto it = p.find(QString::fromLatin1(key));
  if (it == p.end()) return fallback;
  const double* v = std::get_if<double>(&it->second);
  return v && std::isfinite(*v) ? *v : fallback;
}

// apply `a`, then `b`
ColorMatrix compose(const ColorMatrix& a, const ColorMatrix& b) {
  ColorMatrix r{};
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) r[i][j] = b[i][0] * a[0][j] + b[i][1] * a[1][j] + b[i][2] * a[2][j];
    r[i][3] = b[i][0] * a[0][3] + b[i][1] * a[1][3] + b[i][2] * a[2][3] + b[i][3];
  }
  return r;
}

ColorMatrix fromM3(const M3& m) {
  ColorMatrix r{};
  for (size_t i = 0; i < 3; ++i) {
    for (size_t j = 0; j < 3; ++j) r[i][j] = static_cast<float>(m[i][j]);
  }
  return r;
}

} // namespace

ColorMatrix effectsToColorMatrix(const std::vector<Effect>& effects, bool* hasBlur) {
  ColorMatrix acc = kIdentityColor;
  if (hasBlur) *hasBlur = false;
  for (const Effect& e : effects) {
    ColorMatrix step = kIdentityColor;
    const QString& type = e.type;
    if (type == QLatin1String("brightness")) {
      const auto b = static_cast<float>(std::max(0.0, 1 + num(e.params, "amount", 0)));
      step = {{{b, 0, 0, 0}, {0, b, 0, 0}, {0, 0, b, 0}}};
    } else if (type == QLatin1String("contrast")) {
      const auto c = static_cast<float>(num(e.params, "amount", 1));
      const float o = 0.5f - 0.5f * c;
      step = {{{c, 0, 0, o}, {0, c, 0, o}, {0, 0, c, o}}};
    } else if (type == QLatin1String("saturation")) {
      const double s = num(e.params, "amount", 1);
      step = fromM3({{{0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s},
                      {0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s},
                      {0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s}}});
    } else if (type == QLatin1String("hueRotate")) {
      const double deg = num(e.params, "degrees", 0);
      if (deg == 0) continue;
      const double c = std::cos(qDegreesToRadians(deg));
      const double s = std::sin(qDegreesToRadians(deg));
      step = fromM3({{{0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715, 0.072 - c * 0.072 + s * 0.928},
                      {0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283},
                      {0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715, 0.072 + c * 0.928 + s * 0.072}}});
    } else if (type == QLatin1String("grayscale")) {
      const double k = 1 - std::clamp(num(e.params, "amount", 0), 0.0, 1.0);
      step = fromM3({{{0.2126 + 0.7874 * k, 0.7152 - 0.7152 * k, 0.0722 - 0.0722 * k},
                      {0.2126 - 0.2126 * k, 0.7152 + 0.2848 * k, 0.0722 - 0.0722 * k},
                      {0.2126 - 0.2126 * k, 0.7152 - 0.7152 * k, 0.0722 + 0.9278 * k}}});
    } else if (type == QLatin1String("sepia")) {
      const double k = 1 - std::clamp(num(e.params, "amount", 0), 0.0, 1.0);
      step = fromM3({{{0.393 + 0.607 * k, 0.769 - 0.769 * k, 0.189 - 0.189 * k},
                      {0.349 - 0.349 * k, 0.686 + 0.314 * k, 0.168 - 0.168 * k},
                      {0.272 - 0.272 * k, 0.534 - 0.534 * k, 0.131 + 0.869 * k}}});
    } else if (type == QLatin1String("blur")) {
      if (hasBlur && num(e.params, "radiusPx", 0) > 0) *hasBlur = true;
      continue;
    } else {
      continue;
    }
    acc = compose(acc, step);
  }
  return acc;
}

ColorMatrix yuvToRgb(double kr, double kb, bool fullRange, int bitDepth) {
  const double kg = 1 - kr - kb;
  const bool wide = bitDepth > 8;
  // raw sample (0..1 of the texture format) -> code value: 8 bit textures hold the code directly, P010
  // keeps its 10 bits in the top of 16
  const double code = wide ? 65535.0 / 64.0 : 255.0;
  const double shift = std::pow(2.0, bitDepth - 8);
  double yScale, yOff, cScale, cOff;
  if (fullRange) {
    const double maxCode = std::pow(2.0, bitDepth) - 1;
    yScale = cScale = code / maxCode;
    yOff = 0;
    cOff = std::pow(2.0, bitDepth - 1) / maxCode;
  } else {
    yScale = code / (219 * shift);
    yOff = 16.0 / 219.0;
    cScale = code / (224 * shift);
    cOff = 128.0 / 224.0;
  }
  // y is 0..1 and cb/cr -0.5..0.5 once scaled and offset: y = yScale*Y - yOff, c = cScale*C - cOff
  const double crR = 2 * (1 - kr);
  const double cbB = 2 * (1 - kb);
  const double gCb = -kb / kg * cbB;
  const double gCr = -kr / kg * crR;
  const auto f = [](double v) { return static_cast<float>(v); };
  ColorMatrix m{};
  m[0] = {f(yScale), 0, f(crR * cScale), f(-yOff - crR * cOff)};
  m[1] = {f(yScale), f(gCb * cScale), f(gCr * cScale), f(-yOff - gCb * cOff - gCr * cOff)};
  m[2] = {f(yScale), f(cbB * cScale), 0, f(-yOff - cbB * cOff)};
  return m;
}

UvMap uvForRotation(int rotation) {
  UvMap m;
  switch (((rotation % 360) + 360) % 360) {
  case 90: // stored (us,vs) shows at (1-vs, us), so us = vd, vs = 1-ud
    m.x = {0, 1, 0};
    m.y = {-1, 0, 1};
    break;
  case 180:
    m.x = {-1, 0, 1};
    m.y = {0, -1, 1};
    break;
  case 270: // stored (us,vs) shows at (vs, 1-us), so us = 1-vd, vs = ud
    m.x = {0, -1, 1};
    m.y = {1, 0, 0};
    break;
  default: break;
  }
  return m;
}

} // namespace sf::render
