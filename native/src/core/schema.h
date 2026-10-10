#pragma once

#include "core/json_reader.h"
#include "core/time.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <array>
#include <map>
#include <optional>
#include <variant>
#include <vector>

// C++ mirror of packages/schema (zod). Field names, defaults and ranges follow the TS source so a
// project document written by either app loads in the other. Defaults that zod applies with
// .default() are default member initialisers here and are also applied when parsing JSON.
// Unknown JSON keys are dropped on load, as zod's default object strictness does.
namespace sf {

// ---------- enums ----------

#define SF_ENUM_TABLE(E, ...)                                                       \
  template <> struct EnumTable<E> {                                                 \
    static constexpr std::array values = __VA_ARGS__;                               \
  }

enum class Easing { Linear, Hold, EaseIn, EaseOut, EaseInOut };
SF_ENUM_TABLE(Easing, (std::array<std::pair<Easing, std::string_view>, 5>{{
    {Easing::Linear, "linear"}, {Easing::Hold, "hold"}, {Easing::EaseIn, "easeIn"},
    {Easing::EaseOut, "easeOut"}, {Easing::EaseInOut, "easeInOut"}}}));

enum class MaskShape { Rect, Ellipse, Path };
SF_ENUM_TABLE(MaskShape, (std::array<std::pair<MaskShape, std::string_view>, 3>{{
    {MaskShape::Rect, "rect"}, {MaskShape::Ellipse, "ellipse"}, {MaskShape::Path, "path"}}}));

enum class TrackingStatus { None, Queued, Running, Done, Failed };
SF_ENUM_TABLE(TrackingStatus, (std::array<std::pair<TrackingStatus, std::string_view>, 5>{{
    {TrackingStatus::None, "none"}, {TrackingStatus::Queued, "queued"}, {TrackingStatus::Running, "running"},
    {TrackingStatus::Done, "done"}, {TrackingStatus::Failed, "failed"}}}));

// Order matches the alternatives of ItemProps, so ItemProps::index() is the ItemType.
enum class ItemType { Video, Audio, Image, Text, Caption, Shape, MotionGraphic };
SF_ENUM_TABLE(ItemType, (std::array<std::pair<ItemType, std::string_view>, 7>{{
    {ItemType::Video, "video"}, {ItemType::Audio, "audio"}, {ItemType::Image, "image"},
    {ItemType::Text, "text"}, {ItemType::Caption, "caption"}, {ItemType::Shape, "shape"},
    {ItemType::MotionGraphic, "motionGraphic"}}}));

// Where overlapping layers are blended: scene-linear light (physically right, the native default) or
// display-encoded sRGB values as the Electron canvas does (see ColorManagement).
enum class BlendSpace { Linear, Display };
SF_ENUM_TABLE(BlendSpace, (std::array<std::pair<BlendSpace, std::string_view>, 2>{{
    {BlendSpace::Linear, "linear"}, {BlendSpace::Display, "display"}}}));

enum class TextAlign { Left, Center, Right };
SF_ENUM_TABLE(TextAlign, (std::array<std::pair<TextAlign, std::string_view>, 3>{{
    {TextAlign::Left, "left"}, {TextAlign::Center, "center"}, {TextAlign::Right, "right"}}}));

enum class CaptionHighlight { None, ActiveWord, WordBg };
SF_ENUM_TABLE(CaptionHighlight, (std::array<std::pair<CaptionHighlight, std::string_view>, 3>{{
    {CaptionHighlight::None, "none"}, {CaptionHighlight::ActiveWord, "active-word"},
    {CaptionHighlight::WordBg, "word-bg"}}}));

enum class CaptionMode { Word, Phrase };
SF_ENUM_TABLE(CaptionMode, (std::array<std::pair<CaptionMode, std::string_view>, 2>{{
    {CaptionMode::Word, "word"}, {CaptionMode::Phrase, "phrase"}}}));

enum class ShapeKind { Rect, Ellipse, Triangle };
SF_ENUM_TABLE(ShapeKind, (std::array<std::pair<ShapeKind, std::string_view>, 3>{{
    {ShapeKind::Rect, "rect"}, {ShapeKind::Ellipse, "ellipse"}, {ShapeKind::Triangle, "triangle"}}}));

enum class TrackKind { Video, Audio, Overlay, Text };
SF_ENUM_TABLE(TrackKind, (std::array<std::pair<TrackKind, std::string_view>, 4>{{
    {TrackKind::Video, "video"}, {TrackKind::Audio, "audio"}, {TrackKind::Overlay, "overlay"},
    {TrackKind::Text, "text"}}}));

enum class AssetKind { Video, Audio, Image };
SF_ENUM_TABLE(AssetKind, (std::array<std::pair<AssetKind, std::string_view>, 3>{{
    {AssetKind::Video, "video"}, {AssetKind::Audio, "audio"}, {AssetKind::Image, "image"}}}));

enum class AssetStatus { Importing, Processing, Analyzed, Failed, Missing };
SF_ENUM_TABLE(AssetStatus, (std::array<std::pair<AssetStatus, std::string_view>, 5>{{
    {AssetStatus::Importing, "importing"}, {AssetStatus::Processing, "processing"},
    {AssetStatus::Analyzed, "analyzed"}, {AssetStatus::Failed, "failed"}, {AssetStatus::Missing, "missing"}}}));

enum class TrimEdge { In, Out };
SF_ENUM_TABLE(TrimEdge, (std::array<std::pair<TrimEdge, std::string_view>, 2>{{
    {TrimEdge::In, "in"}, {TrimEdge::Out, "out"}}}));

#undef SF_ENUM_TABLE

// ---------- time, transform, effects, masks ----------

struct Keyframe {
  Frame frame = 0;
  double value = 0;
  Easing easing = Easing::Linear;
  bool operator==(const Keyframe&) const = default;
};
using KeyframeList = std::vector<Keyframe>;
using KeyframeMap = std::map<QString, KeyframeList>;

// No zod defaults on purpose (see transform.ts): sparse patches keep inverses exact. The
// defaults here are DEFAULT_TRANSFORM, applied when items are created.
struct Transform {
  double x = 0;
  double y = 0;
  double scale = 1;
  double scaleX = 1;
  double scaleY = 1;
  double rotation = 0;
  double opacity = 1;
  bool operator==(const Transform&) const = default;
};

struct TransformPatch {
  std::optional<double> x, y, scale, scaleX, scaleY, rotation, opacity;
  bool operator==(const TransformPatch&) const = default;
};

using ParamValue = std::variant<double, QString, bool>;
using EffectParams = std::map<QString, ParamValue>;

struct Effect {
  QString id;
  QString type;
  EffectParams params;
  bool operator==(const Effect&) const = default;
};

struct Point {
  double x = 0;
  double y = 0;
  bool operator==(const Point&) const = default;
};

struct TrackingSample {
  Frame frame = 0;
  double x = 0;
  double y = 0;
  bool operator==(const TrackingSample&) const = default;
};

struct MaskTracking {
  QString assetId;
  TrackingStatus status = TrackingStatus::None;
  std::vector<TrackingSample> positions;
  bool operator==(const MaskTracking&) const = default;
};

struct Mask {
  QString id;
  MaskShape shape = MaskShape::Rect;
  std::optional<std::vector<Point>> path;
  double feather = 0;
  bool invert = false;
  std::optional<KeyframeMap> keyframes;
  std::optional<MaskTracking> tracking;
  bool operator==(const Mask&) const = default;
};

// ---------- item props ----------

struct TextStyle {
  QString fontFamily;
  double fontSize = 0;
  std::int64_t fontWeight = 700;
  QString color;
  std::optional<QString> strokeColor;
  double strokeWidth = 0;
  std::optional<QString> backgroundColor;
  TextAlign align = TextAlign::Center;
  double lineHeight = 1.2;
  double letterSpacing = 0;
  bool uppercase = false;
  double padding = 0;
  double borderRadius = 0;
  bool operator==(const TextStyle&) const = default;
};

struct CaptionStyle : TextStyle {
  CaptionHighlight highlight = CaptionHighlight::ActiveWord;
  QString highlightColor = QStringLiteral("#fbbf24");
  double placementY = 0.82; // 0 = top of canvas, 1 = bottom
  std::int64_t maxCharsPerLine = 28;
  bool operator==(const CaptionStyle&) const = default;
};

struct TranscriptWord {
  QString w;
  Ms startMs = 0;
  Ms endMs = 0;
  std::optional<double> conf;
  std::optional<QString> speaker;
  bool operator==(const TranscriptWord&) const = default;
};

struct TimeRemapPoint {
  Frame frame = 0;       // timeline frame, item-local
  Frame sourceFrame = 0; // source-local
  bool operator==(const TimeRemapPoint&) const = default;
};

struct FadeProps {
  Frame fadeInFrames = 0;
  Frame fadeOutFrames = 0;
  bool operator==(const FadeProps&) const = default;
};
struct VideoProps : FadeProps {
  bool operator==(const VideoProps&) const = default;
};
struct AudioProps : FadeProps {
  bool operator==(const AudioProps&) const = default;
};
struct ImageProps {
  bool operator==(const ImageProps&) const = default;
};
struct TextProps {
  QString text;
  TextStyle style;
  bool operator==(const TextProps&) const = default;
};
struct CaptionProps {
  std::vector<TranscriptWord> words;
  CaptionStyle style;
  CaptionMode mode = CaptionMode::Phrase;
  std::int64_t maxWordsPerCard = 5;
  bool operator==(const CaptionProps&) const = default;
};
struct ShapeProps {
  ShapeKind shape = ShapeKind::Rect;
  QString fill;
  std::optional<QString> stroke;
  double strokeWidth = 0;
  double radius = 0;
  double width = 0; // logical size within the canvas; transform scales it
  double height = 0;
  bool operator==(const ShapeProps&) const = default;
};
// Code is validated and sandboxed at runtime; stored verbatim. inputProps is free-form JSON.
struct MotionGraphicProps {
  QString code;
  QJsonObject inputProps;
  bool operator==(const MotionGraphicProps&) const = default;
};

using ItemProps =
    std::variant<VideoProps, AudioProps, ImageProps, TextProps, CaptionProps, ShapeProps, MotionGraphicProps>;

struct Labels {
  std::optional<QString> name;
  std::optional<QString> color;
  bool operator==(const Labels&) const = default;
};

// Native-only colour management for one item. Absent on the item = today's behaviour (the stream's own
// tags, sRGB-style display-referred video). The Electron app's zod schema strips these keys on load,
// so a project round-tripped through it loses them; the native app reads documents without them fine.
struct ItemColor {
  // OpenColorIO colour space the source pixels are in (e.g. "ARRI LogC3 (EI800)"); absent = by stream tags
  std::optional<QString> inputSpace;
  // LUT file (.cube .3dl .clf .spi3d ...) applied to the source values before `inputSpace` is
  // interpreted; its output is what `inputSpace` describes. See render/color_manager.h.
  std::optional<QString> lutPath;
  double lutIntensity = 1; // 0..1 mix of source and LUT output
  bool operator==(const ItemColor&) const = default;
};

// One timeline item. The zod discriminated union becomes the type-specific `props` variant; the
// shared fields live here.
struct Item {
  QString id;
  QString trackId;
  Frame startFrame = 0;
  Frame durationFrames = 1;
  std::optional<QString> assetId;
  std::optional<Frame> sourceInFrame; // source-local in point; out = in + duration * speed
  double speed = 1;                   // frames per source frame; ignored when timeRemap is non-empty
  std::vector<TimeRemapPoint> timeRemap;
  Transform transform;
  double volume = 1;
  bool muted = false;
  std::vector<Effect> effects;
  std::vector<Mask> masks;
  KeyframeMap keyframes;
  Labels labels;
  std::optional<ItemColor> color; // native-only, see ItemColor
  ItemProps props;

  ItemType type() const { return static_cast<ItemType>(props.index()); }
  bool operator==(const Item&) const = default;
};

// Partial update. `labels` merges per key: an engaged outer optional with an empty inner one
// means "remove this key" (what inverses use). `props` is raw JSON, validated against the item's
// own type when applied, because the type is not known until then.
struct LabelsPatch {
  std::optional<std::optional<QString>> name;
  std::optional<std::optional<QString>> color;
  bool operator==(const LabelsPatch&) const = default;
};

struct ItemPatch {
  std::optional<QString> trackId;
  std::optional<Frame> startFrame;
  std::optional<Frame> durationFrames;
  std::optional<Frame> sourceInFrame;
  std::optional<double> speed;
  std::optional<std::vector<TimeRemapPoint>> timeRemap;
  std::optional<TransformPatch> transform;
  std::optional<double> volume;
  std::optional<bool> muted;
  std::optional<std::vector<Effect>> effects;
  std::optional<std::vector<Mask>> masks;
  std::optional<KeyframeMap> keyframes;
  std::optional<LabelsPatch> labels;
  std::optional<ItemColor> color; // replaces the whole block; a default ItemColor clears it
  std::optional<QJsonValue> props;
  bool operator==(const ItemPatch&) const = default;
};

// ---------- project ----------

struct Track {
  QString id;
  TrackKind kind = TrackKind::Video;
  QString name;
  bool locked = false;
  bool muted = false;
  bool hidden = false;
  bool operator==(const Track&) const = default;
};

struct StyleConfig {
  std::vector<QString> fonts;
  QString primaryColor = QStringLiteral("#fbbf24");
  QString backgroundColor = QStringLiteral("#0a0a0a");
  std::optional<CaptionStyle> captionStyle;
  std::optional<TextStyle> titleStyle;
  bool operator==(const StyleConfig&) const = default;
};

// Native-only project colour management. Absent = defaults: linear-light blending, built-in linear
// Rec.709 working space, sRGB display. The zod schema strips this key (see ItemColor).
struct ColorManagement {
  std::optional<QString> workingSpace; // OpenColorIO scene-linear space (e.g. "ACEScg"); absent = built-in linear Rec.709
  // delivery target: "srgb", "rec709", "rec2100pq", "rec2100hlg", or a display-referred OCIO colour space name
  std::optional<QString> outputSpace;
  std::optional<QString> displayView; // preview transform "Display/View" (e.g. "sRGB - Display/ACES 1.0 - SDR Video")
  BlendSpace blendSpace = BlendSpace::Linear;
  bool operator==(const ColorManagement&) const = default;
};

struct Project {
  QString id;
  QString name;
  std::int64_t fps = 30;
  std::int64_t width = 1920;
  std::int64_t height = 1080;
  std::optional<QString> templateId;
  StyleConfig styleConfig;
  std::optional<QString> referenceAssetId;
  std::optional<ColorManagement> colorManagement; // native-only
  QString createdAt;
  QString updatedAt;
  bool operator==(const Project&) const = default;
};

struct Marker {
  QString id;
  Frame frame = 0;
  QString label;
  std::optional<QString> color;
  bool operator==(const Marker&) const = default;
};

struct TimelineDoc {
  Project project;
  std::vector<Track> tracks;
  std::vector<Item> items;
  std::vector<Marker> markers;
  bool operator==(const TimelineDoc&) const = default;
};

// ---------- assets and analysis ----------

struct AssetMetadata {
  std::optional<QString> location;
  std::optional<QString> capturedAt;
  std::optional<QString> codec;
  bool operator==(const AssetMetadata&) const = default;
};

struct Asset {
  QString id;
  QString projectId;
  AssetKind kind = AssetKind::Video;
  QString path;
  QString originalName;
  std::optional<QString> proxyPath;
  std::optional<QString> thumbPath;
  std::optional<QString> waveformPath;
  AssetStatus status = AssetStatus::Importing;
  std::optional<QString> stage;
  Ms durationMs = 0;
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::optional<double> fps;
  bool hasAudio = false;
  bool hasSpeech = false;
  std::int64_t sizeBytes = 0;
  double mtimeMs = 0;
  std::optional<QString> hash;
  AssetMetadata metadata;
  QString createdAt;
  std::optional<QString> error;
  bool operator==(const Asset&) const = default;
};

struct Transcript {
  QString assetId;
  QString language = QStringLiteral("en");
  std::vector<TranscriptWord> words;
  bool operator==(const Transcript&) const = default;
};

struct Scene {
  QString id;
  QString assetId;
  Ms startMs = 0;
  Ms endMs = 0;
  QString description;
  std::vector<QString> tags;
  std::vector<QString> keyframePaths;
  bool operator==(const Scene&) const = default;
};

struct BeatSection {
  Ms startMs = 0;
  Ms endMs = 0;
  QString label;
  double energy = 0.5;
  bool operator==(const BeatSection&) const = default;
};

struct BeatMap {
  QString assetId;
  double bpm = 0;
  std::vector<double> beatsMs;
  std::vector<double> downbeatsMs;
  std::vector<BeatSection> sections;
  bool operator==(const BeatMap&) const = default;
};

struct ProjectBundle {
  TimelineDoc doc;
  std::vector<Asset> assets;
  std::vector<Transcript> transcripts;
  std::vector<Scene> scenes;
  std::vector<BeatMap> beatMaps;
  bool operator==(const ProjectBundle&) const = default;
};

// ---------- ids ----------

// Prefixed ids ("itm_0123456789abcdef"): `prefix` is prj, ast, trk, itm, mrk, exp or job.
// Ids are generated where ops are created, never inside apply, so replaying the log is deterministic.
QString newId(const QString& prefix);
bool isValidId(const QString& prefix, const QString& value);

// ---------- JSON ----------

// parse* validate and apply defaults; they throw SchemaError naming the first bad field. to*
// produce what zod's parse + JSON.stringify would (optional fields omitted, defaults explicit).
Keyframe parseKeyframe(const Rd& r);
Effect parseEffect(const Rd& r);
EffectParams parseEffectParams(const Rd& r);
Mask parseMask(const Rd& r);
TextStyle parseTextStyle(const Rd& r);
CaptionStyle parseCaptionStyle(const Rd& r);
TimeRemapPoint parseTimeRemapPoint(const Rd& r);
Marker parseMarker(const Rd& r);
StyleConfig parseStyleConfig(const Rd& r);
Track parseTrack(const Rd& r);
Project parseProject(const Rd& r);
KeyframeMap parseKeyframeMap(const Rd& r);
Transform parseTransform(const Rd& r);
TransformPatch parseTransformPatch(const Rd& r);

// Item props for a given type; `{}` yields the zod defaults for video/audio/image and throws for
// the types with required fields, same as itemPropsSchemas[type].parse({}).
ItemProps parseItemProps(ItemType type, const Rd& r);
// propsOmitted: item.add may leave props out; for video/audio/image that is filled with defaults
// and reported through *omitted, for the others the (empty) props are a placeholder.
Item parseItem(const Rd& r, bool propsOptional = false, bool* omitted = nullptr);
ItemPatch parseItemPatch(const Rd& r);
TimelineDoc parseTimelineDoc(const Rd& r);
Asset parseAsset(const Rd& r);
Transcript parseTranscript(const Rd& r);
Scene parseScene(const Rd& r);
BeatMap parseBeatMap(const Rd& r);
ProjectBundle parseProjectBundle(const Rd& r);

QJsonObject toJson(const Keyframe&);
QJsonObject toJson(const Effect&);
QJsonObject toJson(const EffectParams&);
QJsonObject toJson(const Mask&);
QJsonObject toJson(const TextStyle&);
QJsonObject toJson(const CaptionStyle&);
QJsonObject toJson(const TimeRemapPoint&);
QJsonObject toJson(const Marker&);
QJsonObject toJson(const StyleConfig&);
QJsonObject toJson(const Track&);
QJsonObject toJson(const Project&);
QJsonObject toJson(const Transform&);
QJsonObject toJson(const TransformPatch&);
QJsonObject toJson(const KeyframeMap&);
QJsonObject toJson(const ItemProps&);
QJsonObject toJson(const Item&, bool omitProps = false);
QJsonObject toJson(const ItemPatch&);
QJsonObject toJson(const TimelineDoc&);
QJsonObject toJson(const Asset&);
QJsonObject toJson(const Transcript&);
QJsonObject toJson(const Scene&);
QJsonObject toJson(const BeatMap&);
QJsonObject toJson(const ProjectBundle&);

// Whole-document convenience: text in, text out, and files (saves go through QSaveFile so a crash
// cannot leave a half-written project).
TimelineDoc parseTimelineDocJson(const QByteArray& json);
QByteArray timelineDocToJson(const TimelineDoc& doc, bool indented = false);
TimelineDoc loadTimelineDoc(const QString& path);
void saveTimelineDoc(const QString& path, const TimelineDoc& doc, bool indented = true);
ProjectBundle parseProjectBundleJson(const QByteArray& json);
QByteArray projectBundleToJson(const ProjectBundle& bundle, bool indented = false);
ProjectBundle loadProjectBundle(const QString& path);
void saveProjectBundle(const QString& path, const ProjectBundle& bundle, bool indented = true);

} // namespace sf
