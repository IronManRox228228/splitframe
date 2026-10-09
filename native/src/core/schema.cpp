#include "core/schema.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

namespace sf {

namespace {

constexpr NumBounds kPositive{.min = 0.0, .exclusiveMin = true};
constexpr NumBounds kNonNegative{.min = 0.0};
constexpr IntBounds kIntNonNeg{.min = 0};

// ---------- writing helpers ----------

QJsonValue jv(double v) { return QJsonValue(v); }
QJsonValue jv(Frame v) { return QJsonValue(static_cast<qint64>(v)); }
QJsonValue jv(const QString& v) { return QJsonValue(v); }
QJsonValue jv(bool v) { return QJsonValue(v); }

template <class T> void putOpt(QJsonObject& o, const char* key, const std::optional<T>& v) {
  if (v) o.insert(QLatin1String(key), jv(*v));
}

template <class T, class F> QJsonArray arr(const std::vector<T>& v, F&& f) {
  QJsonArray a;
  for (const T& e : v) a.append(f(e));
  return a;
}

QJsonObject pointJson(const Point& p) { return {{QStringLiteral("x"), p.x}, {QStringLiteral("y"), p.y}}; }

QJsonObject trackingSampleJson(const TrackingSample& s) {
  return {{QStringLiteral("frame"), jv(s.frame)}, {QStringLiteral("x"), s.x}, {QStringLiteral("y"), s.y}};
}

QJsonObject wordJson(const TranscriptWord& w) {
  QJsonObject o{{QStringLiteral("w"), w.w}, {QStringLiteral("startMs"), jv(w.startMs)}, {QStringLiteral("endMs"), jv(w.endMs)}};
  putOpt(o, "conf", w.conf);
  putOpt(o, "speaker", w.speaker);
  return o;
}

void writeTextStyle(QJsonObject& o, const TextStyle& s) {
  o.insert(QStringLiteral("fontFamily"), s.fontFamily);
  o.insert(QStringLiteral("fontSize"), s.fontSize);
  o.insert(QStringLiteral("fontWeight"), jv(s.fontWeight));
  o.insert(QStringLiteral("color"), s.color);
  putOpt(o, "strokeColor", s.strokeColor);
  o.insert(QStringLiteral("strokeWidth"), s.strokeWidth);
  putOpt(o, "backgroundColor", s.backgroundColor);
  o.insert(QStringLiteral("align"), enumName(s.align));
  o.insert(QStringLiteral("lineHeight"), s.lineHeight);
  o.insert(QStringLiteral("letterSpacing"), s.letterSpacing);
  o.insert(QStringLiteral("uppercase"), s.uppercase);
  o.insert(QStringLiteral("padding"), s.padding);
  o.insert(QStringLiteral("borderRadius"), s.borderRadius);
}

QJsonValue paramJson(const ParamValue& v) {
  return std::visit([](const auto& x) { return jv(x); }, v);
}


// ---------- reading helpers ----------

Point parsePoint(const Rd& r) {
  r.requireObject();
  Point p;
  p.x = r.field(QStringLiteral("x")).num();
  p.y = r.field(QStringLiteral("y")).num();
  return p;
}

TrackingSample parseTrackingSample(const Rd& r) {
  r.requireObject();
  TrackingSample s;
  s.frame = r.field(QStringLiteral("frame")).integer();
  s.x = r.field(QStringLiteral("x")).num();
  s.y = r.field(QStringLiteral("y")).num();
  return s;
}

TranscriptWord parseWord(const Rd& r) {
  r.requireObject();
  TranscriptWord w;
  w.w = r.field(QStringLiteral("w")).str();
  w.startMs = r.field(QStringLiteral("startMs")).integer(kIntNonNeg);
  w.endMs = r.field(QStringLiteral("endMs")).integer(kIntNonNeg);
  w.conf = r.field(QStringLiteral("conf")).optNum({.min = 0.0, .max = 1.0});
  w.speaker = r.field(QStringLiteral("speaker")).optStr();
  return w;
}

ParamValue parseParam(const Rd& r) {
  const QJsonValue& v = r.raw();
  if (v.isBool()) return v.toBool();
  if (v.isDouble()) return r.num();
  if (v.isString()) return v.toString();
  if (r.missing()) r.fail(QStringLiteral("Required"));
  r.fail(QStringLiteral("Invalid input: expected number, string or boolean, received %1").arg(Rd::typeName(v)));
}


Labels parseLabels(const Rd& r) {
  Labels l;
  if (r.missing()) return l;
  r.requireObject();
  l.name = r.field(QStringLiteral("name")).optStr();
  l.color = r.field(QStringLiteral("color")).optStr();
  return l;
}

FadeProps parseFade(const Rd& r) {
  r.requireObject();
  FadeProps f;
  f.fadeInFrames = r.field(QStringLiteral("fadeInFrames")).integerOr(0);
  f.fadeOutFrames = r.field(QStringLiteral("fadeOutFrames")).integerOr(0);
  return f;
}

std::vector<TimeRemapPoint> parseRemap(const Rd& r) { return r.listOr(parseTimeRemapPoint); }

} // namespace

// ---------- ids ----------

QString newId(const QString& prefix) {
  const QString hex = QUuid::createUuid().toString(QUuid::Id128);
  return prefix + QLatin1Char('_') + hex.left(16);
}

bool isValidId(const QString& prefix, const QString& s) {
  const QString head = prefix + QLatin1Char('_');
  if (!s.startsWith(head) || s.size() - head.size() < 10) return false;
  for (qsizetype i = head.size(); i < s.size(); ++i) {
    const QChar c = s.at(i);
    if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) return false;
  }
  return true;
}

// ---------- parse ----------

Keyframe parseKeyframe(const Rd& r) {
  r.requireObject();
  Keyframe k;
  k.frame = r.field(QStringLiteral("frame")).integer();
  k.value = r.field(QStringLiteral("value")).num();
  k.easing = r.field(QStringLiteral("easing")).enumOr(Easing::Linear);
  return k;
}

KeyframeMap parseKeyframeMap(const Rd& r) {
  KeyframeMap m;
  if (r.missing()) return m;
  for (const auto& [prop, list] : r.entries()) m.emplace(prop, list.list(parseKeyframe));
  return m;
}

Transform parseTransform(const Rd& r) {
  r.requireObject();
  Transform t;
  t.x = r.field(QStringLiteral("x")).num();
  t.y = r.field(QStringLiteral("y")).num();
  t.scale = r.field(QStringLiteral("scale")).num(kPositive);
  t.scaleX = r.field(QStringLiteral("scaleX")).num(kPositive);
  t.scaleY = r.field(QStringLiteral("scaleY")).num(kPositive);
  t.rotation = r.field(QStringLiteral("rotation")).num();
  t.opacity = r.field(QStringLiteral("opacity")).num({.min = 0.0, .max = 1.0});
  return t;
}

TransformPatch parseTransformPatch(const Rd& r) {
  r.requireObject();
  TransformPatch t;
  t.x = r.field(QStringLiteral("x")).optNum();
  t.y = r.field(QStringLiteral("y")).optNum();
  t.scale = r.field(QStringLiteral("scale")).optNum(kPositive);
  t.scaleX = r.field(QStringLiteral("scaleX")).optNum(kPositive);
  t.scaleY = r.field(QStringLiteral("scaleY")).optNum(kPositive);
  t.rotation = r.field(QStringLiteral("rotation")).optNum();
  t.opacity = r.field(QStringLiteral("opacity")).optNum({.min = 0.0, .max = 1.0});
  return t;
}

EffectParams parseEffectParams(const Rd& r) {
  EffectParams out;
  for (const auto& [k, v] : r.entries()) out.emplace(k, parseParam(v));
  return out;
}

Effect parseEffect(const Rd& r) {
  r.requireObject();
  Effect e;
  e.id = r.field(QStringLiteral("id")).str();
  e.type = r.field(QStringLiteral("type")).str();
  e.params = parseEffectParams(r.field(QStringLiteral("params")));
  return e;
}

Mask parseMask(const Rd& r) {
  r.requireObject();
  Mask m;
  m.id = r.field(QStringLiteral("id")).str();
  m.shape = r.field(QStringLiteral("shape")).enumeration<MaskShape>();
  m.path = r.field(QStringLiteral("path")).opt([](const Rd& p) { return p.list(parsePoint); });
  m.feather = r.field(QStringLiteral("feather")).numOr(0, kNonNegative);
  m.invert = r.field(QStringLiteral("invert")).boolOr(false);
  const Rd kf = r.field(QStringLiteral("keyframes"));
  if (!kf.missing()) m.keyframes = parseKeyframeMap(kf);
  const Rd tr = r.field(QStringLiteral("tracking"));
  if (!tr.missing()) {
    tr.requireObject();
    MaskTracking t;
    t.assetId = tr.field(QStringLiteral("assetId")).str();
    t.status = tr.field(QStringLiteral("status")).enumeration<TrackingStatus>();
    t.positions = tr.field(QStringLiteral("positions")).listOr(parseTrackingSample);
    m.tracking = std::move(t);
  }
  return m;
}

TextStyle parseTextStyle(const Rd& r) {
  r.requireObject();
  TextStyle s;
  s.fontFamily = r.field(QStringLiteral("fontFamily")).str();
  s.fontSize = r.field(QStringLiteral("fontSize")).num(kPositive);
  s.fontWeight = r.field(QStringLiteral("fontWeight")).integerOr(700, {.min = 100, .max = 1000});
  s.color = r.field(QStringLiteral("color")).str();
  s.strokeColor = r.field(QStringLiteral("strokeColor")).optStr();
  s.strokeWidth = r.field(QStringLiteral("strokeWidth")).numOr(0, kNonNegative);
  s.backgroundColor = r.field(QStringLiteral("backgroundColor")).optStr();
  s.align = r.field(QStringLiteral("align")).enumOr(TextAlign::Center);
  s.lineHeight = r.field(QStringLiteral("lineHeight")).numOr(1.2, kPositive);
  s.letterSpacing = r.field(QStringLiteral("letterSpacing")).numOr(0);
  s.uppercase = r.field(QStringLiteral("uppercase")).boolOr(false);
  s.padding = r.field(QStringLiteral("padding")).numOr(0, kNonNegative);
  s.borderRadius = r.field(QStringLiteral("borderRadius")).numOr(0, kNonNegative);
  return s;
}

CaptionStyle parseCaptionStyle(const Rd& r) {
  CaptionStyle s;
  static_cast<TextStyle&>(s) = parseTextStyle(r);
  s.highlight = r.field(QStringLiteral("highlight")).enumOr(CaptionHighlight::ActiveWord);
  s.highlightColor = r.field(QStringLiteral("highlightColor")).strOr(QStringLiteral("#fbbf24"));
  s.placementY = r.field(QStringLiteral("placementY")).numOr(0.82, {.min = 0.0, .max = 1.0});
  s.maxCharsPerLine = r.field(QStringLiteral("maxCharsPerLine")).integerOr(28, {.min = 1});
  return s;
}

TimeRemapPoint parseTimeRemapPoint(const Rd& r) {
  r.requireObject();
  TimeRemapPoint p;
  p.frame = r.field(QStringLiteral("frame")).integer();
  p.sourceFrame = r.field(QStringLiteral("sourceFrame")).integer();
  return p;
}

Marker parseMarker(const Rd& r) {
  r.requireObject();
  Marker m;
  m.id = r.field(QStringLiteral("id")).str();
  m.frame = r.field(QStringLiteral("frame")).integer(kIntNonNeg);
  m.label = r.field(QStringLiteral("label")).str();
  m.color = r.field(QStringLiteral("color")).optStr();
  return m;
}

StyleConfig parseStyleConfig(const Rd& r) {
  StyleConfig s;
  if (r.missing()) return s; // .default({fonts: [], primaryColor, backgroundColor})
  r.requireObject();
  s.fonts = r.field(QStringLiteral("fonts")).listOr([](const Rd& f) { return f.str(); });
  s.primaryColor = r.field(QStringLiteral("primaryColor")).strOr(s.primaryColor);
  s.backgroundColor = r.field(QStringLiteral("backgroundColor")).strOr(s.backgroundColor);
  s.captionStyle = r.field(QStringLiteral("captionStyle")).opt(parseCaptionStyle);
  s.titleStyle = r.field(QStringLiteral("titleStyle")).opt(parseTextStyle);
  return s;
}

Track parseTrack(const Rd& r) {
  r.requireObject();
  Track t;
  t.id = r.field(QStringLiteral("id")).str();
  t.kind = r.field(QStringLiteral("kind")).enumeration<TrackKind>();
  t.name = r.field(QStringLiteral("name")).str();
  t.locked = r.field(QStringLiteral("locked")).boolOr(false);
  t.muted = r.field(QStringLiteral("muted")).boolOr(false);
  t.hidden = r.field(QStringLiteral("hidden")).boolOr(false);
  return t;
}

Project parseProject(const Rd& r) {
  r.requireObject();
  Project p;
  p.id = r.field(QStringLiteral("id")).str();
  p.name = r.field(QStringLiteral("name")).str();
  p.fps = r.field(QStringLiteral("fps")).integerOr(30, {.min = 1});
  p.width = r.field(QStringLiteral("width")).integerOr(1920, {.min = 1});
  p.height = r.field(QStringLiteral("height")).integerOr(1080, {.min = 1});
  p.templateId = r.field(QStringLiteral("templateId")).optStr();
  p.styleConfig = parseStyleConfig(r.field(QStringLiteral("styleConfig")));
  p.referenceAssetId = r.field(QStringLiteral("referenceAssetId")).optStr();
  p.createdAt = r.field(QStringLiteral("createdAt")).str();
  p.updatedAt = r.field(QStringLiteral("updatedAt")).str();
  return p;
}

ItemProps parseItemProps(ItemType type, const Rd& r) {
  r.requireObject();
  switch (type) {
    case ItemType::Video: {
      VideoProps p;
      static_cast<FadeProps&>(p) = parseFade(r);
      return p;
    }
    case ItemType::Audio: {
      AudioProps p;
      static_cast<FadeProps&>(p) = parseFade(r);
      return p;
    }
    case ItemType::Image: return ImageProps{};
    case ItemType::Text: {
      TextProps p;
      p.text = r.field(QStringLiteral("text")).str();
      p.style = parseTextStyle(r.field(QStringLiteral("style")));
      return p;
    }
    case ItemType::Caption: {
      CaptionProps p;
      p.words = r.field(QStringLiteral("words")).list(parseWord);
      p.style = parseCaptionStyle(r.field(QStringLiteral("style")));
      p.mode = r.field(QStringLiteral("mode")).enumOr(CaptionMode::Phrase);
      p.maxWordsPerCard = r.field(QStringLiteral("maxWordsPerCard")).integerOr(5, {.min = 1});
      return p;
    }
    case ItemType::Shape: {
      ShapeProps p;
      p.shape = r.field(QStringLiteral("shape")).enumeration<ShapeKind>();
      p.fill = r.field(QStringLiteral("fill")).str();
      p.stroke = r.field(QStringLiteral("stroke")).optStr();
      p.strokeWidth = r.field(QStringLiteral("strokeWidth")).numOr(0, kNonNegative);
      p.radius = r.field(QStringLiteral("radius")).numOr(0, kNonNegative);
      p.width = r.field(QStringLiteral("width")).num(kPositive);
      p.height = r.field(QStringLiteral("height")).num(kPositive);
      return p;
    }
    case ItemType::MotionGraphic: {
      MotionGraphicProps p;
      p.code = r.field(QStringLiteral("code")).str();
      const Rd ip = r.field(QStringLiteral("inputProps"));
      if (!ip.missing()) {
        ip.requireObject();
        p.inputProps = ip.raw().toObject();
      }
      return p;
    }
  }
  r.fail(QStringLiteral("Invalid item type"));
}

Item parseItem(const Rd& r, bool propsOptional, bool* omitted) {
  r.requireObject();
  if (omitted) *omitted = false;
  const Rd typeRd = r.field(QStringLiteral("type"));
  if (typeRd.missing() || !typeRd.raw().isString()) {
    QStringList names;
    for (const auto& entry : EnumTable<ItemType>::values) names << QStringLiteral("'%1'").arg(enumName(entry.first));
    typeRd.fail(QStringLiteral("Invalid discriminator value. Expected %1").arg(names.join(QStringLiteral(" | "))));
  }
  const ItemType type = typeRd.enumeration<ItemType>();

  Item it;
  it.id = r.field(QStringLiteral("id")).str();
  it.trackId = r.field(QStringLiteral("trackId")).str();
  it.startFrame = r.field(QStringLiteral("startFrame")).integer(kIntNonNeg);
  it.durationFrames = r.field(QStringLiteral("durationFrames")).integer({.min = 1});
  it.assetId = r.field(QStringLiteral("assetId")).optStr();
  it.sourceInFrame = r.field(QStringLiteral("sourceInFrame")).optInteger(kIntNonNeg);
  it.speed = r.field(QStringLiteral("speed")).numOr(1, {.min = 0.1, .max = 16.0});
  it.timeRemap = parseRemap(r.field(QStringLiteral("timeRemap")));
  it.transform = parseTransform(r.field(QStringLiteral("transform")));
  it.volume = r.field(QStringLiteral("volume")).numOr(1, kNonNegative);
  it.muted = r.field(QStringLiteral("muted")).boolOr(false);
  it.effects = r.field(QStringLiteral("effects")).listOr(parseEffect);
  it.masks = r.field(QStringLiteral("masks")).listOr(parseMask);
  it.keyframes = parseKeyframeMap(r.field(QStringLiteral("keyframes")));
  it.labels = parseLabels(r.field(QStringLiteral("labels")));

  const Rd props = r.field(QStringLiteral("props"));
  if (props.missing() && propsOptional) {
    bool defaultable = type == ItemType::Video || type == ItemType::Audio || type == ItemType::Image;
    if (defaultable) {
      it.props = parseItemProps(type, Rd(QJsonObject{}, props.path()));
    } else {
      // placeholder with the right alternative; the apply step fails it like itemPropsSchemas.parse({})
      switch (type) {
        case ItemType::Text: it.props = TextProps{}; break;
        case ItemType::Caption: it.props = CaptionProps{}; break;
        case ItemType::Shape: it.props = ShapeProps{}; break;
        default: it.props = MotionGraphicProps{}; break;
      }
    }
    if (omitted) *omitted = true;
  } else {
    it.props = parseItemProps(type, props);
  }
  return it;
}

ItemPatch parseItemPatch(const Rd& r) {
  r.requireObject();
  ItemPatch p;
  p.trackId = r.field(QStringLiteral("trackId")).optStr();
  p.startFrame = r.field(QStringLiteral("startFrame")).optInteger();
  p.durationFrames = r.field(QStringLiteral("durationFrames")).optInteger({.min = 1});
  p.sourceInFrame = r.field(QStringLiteral("sourceInFrame")).optInteger(kIntNonNeg);
  p.speed = r.field(QStringLiteral("speed")).optNum({.min = 0.1, .max = 16.0});
  p.timeRemap = r.field(QStringLiteral("timeRemap")).opt([](const Rd& x) { return x.list(parseTimeRemapPoint); });
  p.transform = r.field(QStringLiteral("transform")).opt(parseTransformPatch);
  p.volume = r.field(QStringLiteral("volume")).optNum(kNonNegative);
  p.muted = r.field(QStringLiteral("muted")).optBool();
  p.effects = r.field(QStringLiteral("effects")).opt([](const Rd& x) { return x.list(parseEffect); });
  p.masks = r.field(QStringLiteral("masks")).opt([](const Rd& x) { return x.list(parseMask); });
  const Rd kf = r.field(QStringLiteral("keyframes"));
  if (!kf.missing()) {
    kf.requireObject();
    p.keyframes = parseKeyframeMap(kf);
  }
  const Rd labels = r.field(QStringLiteral("labels"));
  if (!labels.missing()) {
    labels.requireObject();
    LabelsPatch lp;
    if (auto n = labels.field(QStringLiteral("name")).optStr()) lp.name = std::optional<QString>(*n);
    if (auto c = labels.field(QStringLiteral("color")).optStr()) lp.color = std::optional<QString>(*c);
    p.labels = lp;
  }
  const Rd props = r.field(QStringLiteral("props"));
  if (!props.missing()) p.props = props.raw();
  return p;
}

TimelineDoc parseTimelineDoc(const Rd& r) {
  r.requireObject();
  TimelineDoc d;
  d.project = parseProject(r.field(QStringLiteral("project")));
  d.tracks = r.field(QStringLiteral("tracks")).list(parseTrack);
  d.items = r.field(QStringLiteral("items")).list([](const Rd& x) { return parseItem(x); });
  d.markers = r.field(QStringLiteral("markers")).list(parseMarker);
  return d;
}

Asset parseAsset(const Rd& r) {
  r.requireObject();
  Asset a;
  a.id = r.field(QStringLiteral("id")).str();
  a.projectId = r.field(QStringLiteral("projectId")).str();
  a.kind = r.field(QStringLiteral("kind")).enumeration<AssetKind>();
  a.path = r.field(QStringLiteral("path")).str();
  a.originalName = r.field(QStringLiteral("originalName")).str();
  a.proxyPath = r.field(QStringLiteral("proxyPath")).optStr();
  a.thumbPath = r.field(QStringLiteral("thumbPath")).optStr();
  a.waveformPath = r.field(QStringLiteral("waveformPath")).optStr();
  a.status = r.field(QStringLiteral("status")).enumOr(AssetStatus::Importing);
  a.stage = r.field(QStringLiteral("stage")).optStr();
  a.durationMs = r.field(QStringLiteral("durationMs")).integerOr(0, kIntNonNeg);
  a.width = r.field(QStringLiteral("width")).integerOr(0, kIntNonNeg);
  a.height = r.field(QStringLiteral("height")).integerOr(0, kIntNonNeg);
  a.fps = r.field(QStringLiteral("fps")).optNum();
  a.hasAudio = r.field(QStringLiteral("hasAudio")).boolOr(false);
  a.hasSpeech = r.field(QStringLiteral("hasSpeech")).boolOr(false);
  a.sizeBytes = r.field(QStringLiteral("sizeBytes")).integerOr(0, kIntNonNeg);
  a.mtimeMs = r.field(QStringLiteral("mtimeMs")).numOr(0);
  a.hash = r.field(QStringLiteral("hash")).optStr();
  const Rd md = r.field(QStringLiteral("metadata"));
  if (!md.missing()) {
    md.requireObject();
    a.metadata.location = md.field(QStringLiteral("location")).optStr();
    a.metadata.capturedAt = md.field(QStringLiteral("capturedAt")).optStr();
    a.metadata.codec = md.field(QStringLiteral("codec")).optStr();
  }
  a.createdAt = r.field(QStringLiteral("createdAt")).str();
  a.error = r.field(QStringLiteral("error")).optStr();
  return a;
}

Transcript parseTranscript(const Rd& r) {
  r.requireObject();
  Transcript t;
  t.assetId = r.field(QStringLiteral("assetId")).str();
  t.language = r.field(QStringLiteral("language")).strOr(QStringLiteral("en"));
  t.words = r.field(QStringLiteral("words")).list(parseWord);
  return t;
}

Scene parseScene(const Rd& r) {
  r.requireObject();
  Scene s;
  s.id = r.field(QStringLiteral("id")).str();
  s.assetId = r.field(QStringLiteral("assetId")).str();
  s.startMs = r.field(QStringLiteral("startMs")).integer(kIntNonNeg);
  s.endMs = r.field(QStringLiteral("endMs")).integer(kIntNonNeg);
  s.description = r.field(QStringLiteral("description")).str();
  s.tags = r.field(QStringLiteral("tags")).listOr([](const Rd& x) { return x.str(); });
  s.keyframePaths = r.field(QStringLiteral("keyframePaths")).listOr([](const Rd& x) { return x.str(); });
  return s;
}

BeatMap parseBeatMap(const Rd& r) {
  r.requireObject();
  BeatMap b;
  b.assetId = r.field(QStringLiteral("assetId")).str();
  b.bpm = r.field(QStringLiteral("bpm")).num();
  b.beatsMs = r.field(QStringLiteral("beatsMs")).list([](const Rd& x) { return x.num(); });
  b.downbeatsMs = r.field(QStringLiteral("downbeatsMs")).list([](const Rd& x) { return x.num(); });
  b.sections = r.field(QStringLiteral("sections")).list([](const Rd& x) {
    x.requireObject();
    BeatSection s;
    s.startMs = x.field(QStringLiteral("startMs")).integer(kIntNonNeg);
    s.endMs = x.field(QStringLiteral("endMs")).integer(kIntNonNeg);
    s.label = x.field(QStringLiteral("label")).str();
    s.energy = x.field(QStringLiteral("energy")).numOr(0.5, {.min = 0.0, .max = 1.0});
    return s;
  });
  return b;
}

ProjectBundle parseProjectBundle(const Rd& r) {
  r.requireObject();
  ProjectBundle b;
  b.doc = parseTimelineDoc(r.field(QStringLiteral("doc")));
  b.assets = r.field(QStringLiteral("assets")).list(parseAsset);
  b.transcripts = r.field(QStringLiteral("transcripts")).list(parseTranscript);
  b.scenes = r.field(QStringLiteral("scenes")).list(parseScene);
  b.beatMaps = r.field(QStringLiteral("beatMaps")).list(parseBeatMap);
  return b;
}

// ---------- write ----------

QJsonObject toJson(const Keyframe& k) {
  return {{QStringLiteral("frame"), jv(k.frame)}, {QStringLiteral("value"), k.value},
          {QStringLiteral("easing"), enumName(k.easing)}};
}

QJsonObject toJson(const KeyframeMap& m) {
  QJsonObject o;
  for (const auto& [prop, list] : m) o.insert(prop, arr(list, [](const Keyframe& k) { return toJson(k); }));
  return o;
}

QJsonObject toJson(const Transform& t) {
  return {{QStringLiteral("x"), t.x},         {QStringLiteral("y"), t.y},
          {QStringLiteral("scale"), t.scale}, {QStringLiteral("scaleX"), t.scaleX},
          {QStringLiteral("scaleY"), t.scaleY}, {QStringLiteral("rotation"), t.rotation},
          {QStringLiteral("opacity"), t.opacity}};
}

QJsonObject toJson(const TransformPatch& t) {
  QJsonObject o;
  putOpt(o, "x", t.x);
  putOpt(o, "y", t.y);
  putOpt(o, "scale", t.scale);
  putOpt(o, "scaleX", t.scaleX);
  putOpt(o, "scaleY", t.scaleY);
  putOpt(o, "rotation", t.rotation);
  putOpt(o, "opacity", t.opacity);
  return o;
}

QJsonObject toJson(const EffectParams& p) {
  QJsonObject o;
  for (const auto& [k, v] : p) o.insert(k, paramJson(v));
  return o;
}

QJsonObject toJson(const Effect& e) {
  return {{QStringLiteral("id"), e.id}, {QStringLiteral("type"), e.type}, {QStringLiteral("params"), toJson(e.params)}};
}

QJsonObject toJson(const Mask& m) {
  QJsonObject o{{QStringLiteral("id"), m.id},
                {QStringLiteral("shape"), enumName(m.shape)},
                {QStringLiteral("feather"), m.feather},
                {QStringLiteral("invert"), m.invert}};
  if (m.path) o.insert(QStringLiteral("path"), arr(*m.path, pointJson));
  if (m.keyframes) o.insert(QStringLiteral("keyframes"), toJson(*m.keyframes));
  if (m.tracking) {
    o.insert(QStringLiteral("tracking"),
             QJsonObject{{QStringLiteral("assetId"), m.tracking->assetId},
                         {QStringLiteral("status"), enumName(m.tracking->status)},
                         {QStringLiteral("positions"), arr(m.tracking->positions, trackingSampleJson)}});
  }
  return o;
}

QJsonObject toJson(const TextStyle& s) {
  QJsonObject o;
  writeTextStyle(o, s);
  return o;
}

QJsonObject toJson(const CaptionStyle& s) {
  QJsonObject o;
  writeTextStyle(o, s);
  o.insert(QStringLiteral("highlight"), enumName(s.highlight));
  o.insert(QStringLiteral("highlightColor"), s.highlightColor);
  o.insert(QStringLiteral("placementY"), s.placementY);
  o.insert(QStringLiteral("maxCharsPerLine"), jv(s.maxCharsPerLine));
  return o;
}

QJsonObject toJson(const TimeRemapPoint& p) {
  return {{QStringLiteral("frame"), jv(p.frame)}, {QStringLiteral("sourceFrame"), jv(p.sourceFrame)}};
}

QJsonObject toJson(const Marker& m) {
  QJsonObject o{{QStringLiteral("id"), m.id}, {QStringLiteral("frame"), jv(m.frame)}, {QStringLiteral("label"), m.label}};
  putOpt(o, "color", m.color);
  return o;
}

QJsonObject toJson(const StyleConfig& s) {
  QJsonObject o{{QStringLiteral("fonts"), arr(s.fonts, [](const QString& f) { return QJsonValue(f); })},
                {QStringLiteral("primaryColor"), s.primaryColor},
                {QStringLiteral("backgroundColor"), s.backgroundColor}};
  if (s.captionStyle) o.insert(QStringLiteral("captionStyle"), toJson(*s.captionStyle));
  if (s.titleStyle) o.insert(QStringLiteral("titleStyle"), toJson(*s.titleStyle));
  return o;
}

QJsonObject toJson(const Track& t) {
  return {{QStringLiteral("id"), t.id},         {QStringLiteral("kind"), enumName(t.kind)},
          {QStringLiteral("name"), t.name},     {QStringLiteral("locked"), t.locked},
          {QStringLiteral("muted"), t.muted},   {QStringLiteral("hidden"), t.hidden}};
}

QJsonObject toJson(const Project& p) {
  QJsonObject o{{QStringLiteral("id"), p.id},
                {QStringLiteral("name"), p.name},
                {QStringLiteral("fps"), jv(p.fps)},
                {QStringLiteral("width"), jv(p.width)},
                {QStringLiteral("height"), jv(p.height)},
                {QStringLiteral("styleConfig"), toJson(p.styleConfig)},
                {QStringLiteral("createdAt"), p.createdAt},
                {QStringLiteral("updatedAt"), p.updatedAt}};
  putOpt(o, "templateId", p.templateId);
  putOpt(o, "referenceAssetId", p.referenceAssetId);
  return o;
}

QJsonObject toJson(const ItemProps& props) {
  struct V {
    QJsonObject operator()(const FadeProps& p) const {
      return {{QStringLiteral("fadeInFrames"), jv(p.fadeInFrames)}, {QStringLiteral("fadeOutFrames"), jv(p.fadeOutFrames)}};
    }
    QJsonObject operator()(const ImageProps&) const { return {}; }
    QJsonObject operator()(const TextProps& p) const {
      return {{QStringLiteral("text"), p.text}, {QStringLiteral("style"), toJson(p.style)}};
    }
    QJsonObject operator()(const CaptionProps& p) const {
      return {{QStringLiteral("words"), arr(p.words, wordJson)},
              {QStringLiteral("style"), toJson(p.style)},
              {QStringLiteral("mode"), enumName(p.mode)},
              {QStringLiteral("maxWordsPerCard"), jv(p.maxWordsPerCard)}};
    }
    QJsonObject operator()(const ShapeProps& p) const {
      QJsonObject o{{QStringLiteral("shape"), enumName(p.shape)},
                    {QStringLiteral("fill"), p.fill},
                    {QStringLiteral("strokeWidth"), p.strokeWidth},
                    {QStringLiteral("radius"), p.radius},
                    {QStringLiteral("width"), p.width},
                    {QStringLiteral("height"), p.height}};
      putOpt(o, "stroke", p.stroke);
      return o;
    }
    QJsonObject operator()(const MotionGraphicProps& p) const {
      return {{QStringLiteral("code"), p.code}, {QStringLiteral("inputProps"), p.inputProps}};
    }
  };
  return std::visit(V{}, props);
}

QJsonObject toJson(const Item& it, bool omitProps) {
  QJsonObject o{{QStringLiteral("id"), it.id},
                {QStringLiteral("trackId"), it.trackId},
                {QStringLiteral("type"), enumName(it.type())},
                {QStringLiteral("startFrame"), jv(it.startFrame)},
                {QStringLiteral("durationFrames"), jv(it.durationFrames)},
                {QStringLiteral("speed"), it.speed},
                {QStringLiteral("timeRemap"), arr(it.timeRemap, [](const TimeRemapPoint& p) { return toJson(p); })},
                {QStringLiteral("transform"), toJson(it.transform)},
                {QStringLiteral("volume"), it.volume},
                {QStringLiteral("muted"), it.muted},
                {QStringLiteral("effects"), arr(it.effects, [](const Effect& e) { return toJson(e); })},
                {QStringLiteral("masks"), arr(it.masks, [](const Mask& m) { return toJson(m); })},
                {QStringLiteral("keyframes"), toJson(it.keyframes)}};
  putOpt(o, "assetId", it.assetId);
  putOpt(o, "sourceInFrame", it.sourceInFrame);
  QJsonObject labels;
  putOpt(labels, "name", it.labels.name);
  putOpt(labels, "color", it.labels.color);
  o.insert(QStringLiteral("labels"), labels);
  if (!omitProps) o.insert(QStringLiteral("props"), toJson(it.props));
  return o;
}

QJsonObject toJson(const ItemPatch& p) {
  QJsonObject o;
  putOpt(o, "trackId", p.trackId);
  putOpt(o, "startFrame", p.startFrame);
  putOpt(o, "durationFrames", p.durationFrames);
  putOpt(o, "sourceInFrame", p.sourceInFrame);
  putOpt(o, "speed", p.speed);
  if (p.timeRemap) o.insert(QStringLiteral("timeRemap"), arr(*p.timeRemap, [](const TimeRemapPoint& x) { return toJson(x); }));
  if (p.transform) o.insert(QStringLiteral("transform"), toJson(*p.transform));
  putOpt(o, "volume", p.volume);
  putOpt(o, "muted", p.muted);
  if (p.effects) o.insert(QStringLiteral("effects"), arr(*p.effects, [](const Effect& x) { return toJson(x); }));
  if (p.masks) o.insert(QStringLiteral("masks"), arr(*p.masks, [](const Mask& x) { return toJson(x); }));
  if (p.keyframes) o.insert(QStringLiteral("keyframes"), toJson(*p.keyframes));
  if (p.labels) {
    // an engaged-but-empty key is "remove it"; JSON.stringify drops those, so do we
    QJsonObject l;
    if (p.labels->name && *p.labels->name) l.insert(QStringLiteral("name"), **p.labels->name);
    if (p.labels->color && *p.labels->color) l.insert(QStringLiteral("color"), **p.labels->color);
    o.insert(QStringLiteral("labels"), l);
  }
  if (p.props) o.insert(QStringLiteral("props"), *p.props);
  return o;
}

QJsonObject toJson(const TimelineDoc& d) {
  return {{QStringLiteral("project"), toJson(d.project)},
          {QStringLiteral("tracks"), arr(d.tracks, [](const Track& t) { return toJson(t); })},
          {QStringLiteral("items"), arr(d.items, [](const Item& i) { return toJson(i); })},
          {QStringLiteral("markers"), arr(d.markers, [](const Marker& m) { return toJson(m); })}};
}

QJsonObject toJson(const Asset& a) {
  QJsonObject o{{QStringLiteral("id"), a.id},
                {QStringLiteral("projectId"), a.projectId},
                {QStringLiteral("kind"), enumName(a.kind)},
                {QStringLiteral("path"), a.path},
                {QStringLiteral("originalName"), a.originalName},
                {QStringLiteral("status"), enumName(a.status)},
                {QStringLiteral("durationMs"), jv(a.durationMs)},
                {QStringLiteral("width"), jv(a.width)},
                {QStringLiteral("height"), jv(a.height)},
                {QStringLiteral("hasAudio"), a.hasAudio},
                {QStringLiteral("hasSpeech"), a.hasSpeech},
                {QStringLiteral("sizeBytes"), jv(a.sizeBytes)},
                {QStringLiteral("mtimeMs"), a.mtimeMs},
                {QStringLiteral("createdAt"), a.createdAt}};
  putOpt(o, "proxyPath", a.proxyPath);
  putOpt(o, "thumbPath", a.thumbPath);
  putOpt(o, "waveformPath", a.waveformPath);
  putOpt(o, "stage", a.stage);
  putOpt(o, "fps", a.fps);
  putOpt(o, "hash", a.hash);
  putOpt(o, "error", a.error);
  QJsonObject md;
  putOpt(md, "location", a.metadata.location);
  putOpt(md, "capturedAt", a.metadata.capturedAt);
  putOpt(md, "codec", a.metadata.codec);
  o.insert(QStringLiteral("metadata"), md);
  return o;
}

QJsonObject toJson(const Transcript& t) {
  return {{QStringLiteral("assetId"), t.assetId},
          {QStringLiteral("language"), t.language},
          {QStringLiteral("words"), arr(t.words, wordJson)}};
}

QJsonObject toJson(const Scene& s) {
  const auto strs = [](const std::vector<QString>& v) {
    return arr(v, [](const QString& x) { return QJsonValue(x); });
  };
  return {{QStringLiteral("id"), s.id},
          {QStringLiteral("assetId"), s.assetId},
          {QStringLiteral("startMs"), jv(s.startMs)},
          {QStringLiteral("endMs"), jv(s.endMs)},
          {QStringLiteral("description"), s.description},
          {QStringLiteral("tags"), strs(s.tags)},
          {QStringLiteral("keyframePaths"), strs(s.keyframePaths)}};
}

QJsonObject toJson(const BeatMap& b) {
  const auto nums = [](const std::vector<double>& v) { return arr(v, [](double x) { return QJsonValue(x); }); };
  return {{QStringLiteral("assetId"), b.assetId},
          {QStringLiteral("bpm"), b.bpm},
          {QStringLiteral("beatsMs"), nums(b.beatsMs)},
          {QStringLiteral("downbeatsMs"), nums(b.downbeatsMs)},
          {QStringLiteral("sections"), arr(b.sections, [](const BeatSection& s) {
             return QJsonObject{{QStringLiteral("startMs"), jv(s.startMs)},
                                {QStringLiteral("endMs"), jv(s.endMs)},
                                {QStringLiteral("label"), s.label},
                                {QStringLiteral("energy"), s.energy}};
           })}};
}

QJsonObject toJson(const ProjectBundle& b) {
  return {{QStringLiteral("doc"), toJson(b.doc)},
          {QStringLiteral("assets"), arr(b.assets, [](const Asset& x) { return toJson(x); })},
          {QStringLiteral("transcripts"), arr(b.transcripts, [](const Transcript& x) { return toJson(x); })},
          {QStringLiteral("scenes"), arr(b.scenes, [](const Scene& x) { return toJson(x); })},
          {QStringLiteral("beatMaps"), arr(b.beatMaps, [](const BeatMap& x) { return toJson(x); })}};
}

// ---------- whole documents ----------

namespace {

QJsonValue parseText(const QByteArray& json) {
  QJsonParseError err;
  const QJsonDocument d = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError)
    throw SchemaError({}, QStringLiteral("Invalid JSON at offset %1: %2").arg(err.offset).arg(err.errorString()));
  return d.isArray() ? QJsonValue(d.array()) : QJsonValue(d.object());
}

QByteArray readFile(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) throw SchemaError(path, QStringLiteral("Cannot open file: %1").arg(f.errorString()));
  return f.readAll();
}

void writeFile(const QString& path, const QByteArray& bytes) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) throw SchemaError(path, QStringLiteral("Cannot write file: %1").arg(f.errorString()));
  f.write(bytes);
  if (!f.commit()) throw SchemaError(path, QStringLiteral("Cannot write file: %1").arg(f.errorString()));
}

QByteArray dump(const QJsonObject& o, bool indented) {
  return QJsonDocument(o).toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact);
}

} // namespace

TimelineDoc parseTimelineDocJson(const QByteArray& json) { return parseTimelineDoc(Rd(parseText(json))); }
QByteArray timelineDocToJson(const TimelineDoc& doc, bool indented) { return dump(toJson(doc), indented); }
TimelineDoc loadTimelineDoc(const QString& path) { return parseTimelineDocJson(readFile(path)); }
void saveTimelineDoc(const QString& path, const TimelineDoc& doc, bool indented) {
  writeFile(path, timelineDocToJson(doc, indented));
}
ProjectBundle parseProjectBundleJson(const QByteArray& json) { return parseProjectBundle(Rd(parseText(json))); }
QByteArray projectBundleToJson(const ProjectBundle& b, bool indented) { return dump(toJson(b), indented); }
ProjectBundle loadProjectBundle(const QString& path) { return parseProjectBundleJson(readFile(path)); }
void saveProjectBundle(const QString& path, const ProjectBundle& b, bool indented) {
  writeFile(path, projectBundleToJson(b, indented));
}

} // namespace sf
