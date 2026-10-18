#include "engine/style_library.h"

#include "audio/effects.h"
#include "engine/jschema.h"
#include "render/color.h"

#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <set>

#include "render/effects.h"

#include <QDateTime>
#include <QFileInfo>

namespace sf::engine::style {

namespace {

QString q(const char* s) { return QString::fromLatin1(s); }

// ---------------------------------------------------------------- loading

struct Library {
  std::vector<Look> looks;
  std::vector<PacingProfile> pacing;
  std::vector<MixProfile> mix;
  std::vector<AudioPreset> audio;
  std::vector<Recipe> recipes;
  QJsonObject raw[5]; // looks, pacing, mix, audio_presets, recipes as parsed (empty on a parse error)
  QStringList errors; // resource could not be read or parsed
};

const char* const kFiles[5] = {"looks", "pacing", "mix", "audio_presets", "recipes"};
const char* const kKinds[5] = {"look", "pacing", "mix", "audio", "recipe"};

QStringList strList(const QJsonObject& o, const char* key) {
  QStringList l;
  for (const QJsonValue& t : o.value(q(key)).toArray()) l << t.toString();
  return l;
}

double numOr(const QJsonObject& o, const char* key, double dflt) { return o.contains(q(key)) ? o.value(q(key)).toDouble(dflt) : dflt; }

Look parseLook(const QJsonObject& o) {
  Look l;
  l.name = o.value(q("name")).toString();
  l.description = o.value(q("description")).toString();
  for (const QJsonValue& t : o.value(q("tags")).toArray()) l.tags << t.toString();
  l.whenToUse = strList(o, "whenToUse");
  l.avoidWhen = strList(o, "avoidWhen");
  l.params = o.value(q("params")).toObject();
  l.grade = gradeFromJson(l.params);
  l.guardsJson = o.value(q("guards")).toObject();
  l.guards = guardsFromJson(l.guardsJson, {});
  return l;
}

PacingProfile parsePacing(const QJsonObject& o) {
  PacingProfile p;
  p.name = o.value(q("name")).toString();
  p.description = o.value(q("description")).toString();
  p.whenToUse = strList(o, "whenToUse");
  p.avoidWhen = strList(o, "avoidWhen");
  const QJsonObject asl = o.value(q("aslSec")).toObject();
  p.aslMin = asl.value(q("min")).toDouble();
  p.aslMax = asl.value(q("max")).toDouble();
  p.cutsOnBeatMinPct = o.value(q("cutsOnBeatMinPct")).toDouble();
  p.hookMaxSec = o.value(q("hookMaxSec")).toDouble();
  p.minShotSec = o.value(q("minShotSec")).toDouble();
  p.maxShotSec = o.value(q("maxShotSec")).toDouble();
  return p;
}

MixProfile parseMix(const QJsonObject& o) {
  MixProfile m;
  m.name = o.value(q("name")).toString();
  m.description = o.value(q("description")).toString();
  m.whenToUse = strList(o, "whenToUse");
  m.avoidWhen = strList(o, "avoidWhen");
  m.targetLufs = o.value(q("targetLufs")).toDouble(m.targetLufs);
  m.toleranceLu = o.value(q("toleranceLu")).toDouble(m.toleranceLu);
  m.truePeakCeilingDb = o.value(q("truePeakCeilingDb")).toDouble(m.truePeakCeilingDb);
  m.duckMarginDb = o.value(q("duckMarginDb")).toDouble(m.duckMarginDb);
  m.attackMs = o.value(q("attackMs")).toDouble(m.attackMs);
  m.releaseMs = o.value(q("releaseMs")).toDouble(m.releaseMs);
  if (o.contains(q("altTargetLufs"))) m.altTargetLufs = o.value(q("altTargetLufs")).toDouble();
  return m;
}

AudioPreset parseAudioPreset(const QJsonObject& o) {
  AudioPreset a;
  a.id = o.value(q("id")).toString();
  a.name = o.value(q("name")).toString();
  a.category = o.value(q("category")).toString();
  a.description = o.value(q("description")).toString();
  a.whenToUse = strList(o, "whenToUse");
  a.avoidWhen = strList(o, "avoidWhen");
  for (const QJsonValue& v : o.value(q("chain")).toArray()) a.chain.push_back({v.toObject().value(q("type")).toString(), v.toObject().value(q("params")).toObject()});
  return a;
}

RecipeStep parseRecipeStep(const QJsonObject& o) {
  return {o.value(q("command")).toString(), o.value(q("args")).toObject()};
}

Recipe parseRecipe(const QJsonObject& o) {
  Recipe r;
  r.name = o.value(q("name")).toString();
  r.description = o.value(q("description")).toString();
  r.whenToUse = strList(o, "whenToUse");
  r.avoidWhen = strList(o, "avoidWhen");
  for (const QJsonValue& v : o.value(q("steps")).toArray()) r.steps.push_back(parseRecipeStep(v.toObject()));
  for (const QJsonValue& v : o.value(q("verify")).toArray()) r.verify.push_back(parseRecipeStep(v.toObject()));
  return r;
}

Library load() {
  Library l;
  for (int k = 0; k < 5; ++k) {
    QFile f(QStringLiteral(":/sf/style/%1.json").arg(q(kFiles[k])));
    if (!f.open(QIODevice::ReadOnly)) {
      l.errors << QStringLiteral("%1.json: not compiled into the resources").arg(q(kFiles[k]));
      continue;
    }
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
      l.errors << QStringLiteral("%1.json: %2 (offset %3)").arg(q(kFiles[k]), err.errorString()).arg(err.offset);
      continue;
    }
    l.raw[k] = doc.object();
    for (const QJsonValue& v : l.raw[k].value(q("items")).toArray()) {
      const QJsonObject o = v.toObject();
      if (k == 0) l.looks.push_back(parseLook(o));
      else if (k == 1) l.pacing.push_back(parsePacing(o));
      else if (k == 2) l.mix.push_back(parseMix(o));
      else if (k == 3) l.audio.push_back(parseAudioPreset(o));
      else l.recipes.push_back(parseRecipe(o));
    }
  }
  return l;
}

const Library& lib() {
  static const Library l = load();
  return l;
}

// ---------------------------------------------------------------- schema pieces

QJsonObject trio(double lo, double hi) {
  return js::obj({{q("master"), js::number(QString(), lo, hi)}, {q("r"), js::number(QString(), lo, hi)}, {q("g"), js::number(QString(), lo, hi)}, {q("b"), js::number(QString(), lo, hi)}});
}

QJsonObject primariesSchema() {
  return js::obj({{q("exposure"), js::number(QStringLiteral("stops"), -4, 4)},
                  {q("temperature"), js::number(QStringLiteral("-1 cool .. +1 warm"), -1, 1)},
                  {q("tint"), js::number(QStringLiteral("-1 green .. +1 magenta"), -1, 1)},
                  {q("contrast"), js::number(QStringLiteral("0..2, 1 = none"), 0, 2)},
                  {q("pivot"), js::number(QStringLiteral("linear, 0.18 = mid grey"), 0.001, 10)},
                  {q("saturation"), js::number(QStringLiteral("0..2, 1 = none"), 0, 2)},
                  {q("lift"), trio(-0.5, 0.5)},
                  {q("gamma"), trio(0.2, 5)},
                  {q("gain"), trio(0, 4)},
                  {q("offset"), js::obj({{q("r"), js::number(QString(), -1, 1)}, {q("g"), js::number(QString(), -1, 1)}, {q("b"), js::number(QString(), -1, 1)}})}});
}

QJsonObject nonEmptyStr(const QString& desc = {}) {
  QJsonObject o = js::str(desc);
  o.insert(q("minLength"), 1);
  return o;
}

QJsonObject nonEmptyItems(QJsonObject arr) {
  arr.insert(q("minItems"), 1);
  return arr;
}

QJsonObject guardsSchema() {
  QJsonObject band = js::array(js::number(QString(), 0, 360), QStringLiteral("[min, max] vectorscope degrees the mean skin hue must stay inside (skin line 123)"));
  band.insert(q("minItems"), 2);
  band.insert(q("maxItems"), 2);
  return js::obj({{q("maxChromaP95"), js::number(QStringLiteral("display chroma p95 after the grade"), 0, 1)},
                  {q("maxGamutLimitPct"), js::number(QStringLiteral("percent of pixels at HSV saturation >= 0.95"), 0, 100)},
                  {q("maxLiftMaster"), js::number(QStringLiteral("shadow lift of the grade"), 0, 0.5)},
                  {q("blacksCrushedIncrease"), js::number(QStringLiteral("percentage points"), 0, 100)},
                  {q("highlightsClippedIncrease"), js::number(QStringLiteral("percentage points"), 0, 100)},
                  {q("skinHueShiftDeg"), js::number(QStringLiteral("degrees"), 0, 180)},
                  {q("skinMinPct"), js::number(QStringLiteral("percent of pixels"), 0, 100)},
                  {q("exposureShiftStops"), js::number(QStringLiteral("stops beyond the look's own exposure change"), 0, 10)},
                  {q("skinHueBandDeg"), band}});
}

} // namespace

LookGuards guardsFromJson(const QJsonObject& g, LookGuards b) {
  b.maxChromaP95 = numOr(g, "maxChromaP95", b.maxChromaP95);
  b.maxGamutLimitPct = numOr(g, "maxGamutLimitPct", b.maxGamutLimitPct);
  b.maxLiftMaster = numOr(g, "maxLiftMaster", b.maxLiftMaster);
  b.blacksCrushedIncrease = numOr(g, "blacksCrushedIncrease", b.blacksCrushedIncrease);
  b.highlightsClippedIncrease = numOr(g, "highlightsClippedIncrease", b.highlightsClippedIncrease);
  b.skinHueShiftDeg = numOr(g, "skinHueShiftDeg", b.skinHueShiftDeg);
  b.skinMinPct = numOr(g, "skinMinPct", b.skinMinPct);
  b.exposureShiftStops = numOr(g, "exposureShiftStops", b.exposureShiftStops);
  const QJsonArray band = g.value(q("skinHueBandDeg")).toArray();
  if (band.size() == 2) {
    b.skinBandMin = band.at(0).toDouble(b.skinBandMin);
    b.skinBandMax = band.at(1).toDouble(b.skinBandMax);
  }
  return b;
}

// ---------------------------------------------------------------- library access

const std::vector<Look>& looks() { return lib().looks; }
const std::vector<PacingProfile>& pacings() { return lib().pacing; }
const std::vector<MixProfile>& mixes() { return lib().mix; }

const Look* findLook(const QString& name) {
  for (const Look& l : looks())
    if (l.name == name) return &l;
  return nullptr;
}
const PacingProfile* findPacing(const QString& name) {
  for (const PacingProfile& p : pacings())
    if (p.name == name) return &p;
  return nullptr;
}
const std::vector<AudioPreset>& audioPresets() { return lib().audio; }
const AudioPreset* findAudioPreset(const QString& id) {
  for (const AudioPreset& a : audioPresets())
    if (a.id == id) return &a;
  return nullptr;
}
QStringList audioPresetIds() {
  QStringList n;
  for (const AudioPreset& a : audioPresets()) n << a.id;
  return n;
}

const MixProfile* findMix(const QString& name) {
  for (const MixProfile& m : mixes())
    if (m.name == name) return &m;
  return nullptr;
}

const std::vector<Recipe>& recipes() {
  const QString overridePath = qEnvironmentVariable("SF_RECIPES_PATH");
  if (!overridePath.isEmpty()) {
    static QString lastPath;
    static QDateTime lastMod;
    static std::vector<Recipe> customRecipes;
    const QFileInfo fi(overridePath);
    if (lastPath != overridePath || lastMod != fi.lastModified()) {
      customRecipes.clear();
      lastPath = overridePath;
      lastMod = fi.lastModified();
      QFile f(overridePath);
      if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        for (const QJsonValue& v : doc.object().value(q("items")).toArray()) {
          customRecipes.push_back(parseRecipe(v.toObject()));
        }
      }
    }
    return customRecipes;
  }
  return lib().recipes;
}

const Recipe* findRecipe(const QString& name) {
  for (const Recipe& r : recipes())
    if (r.name == name) return &r;
  return nullptr;
}

QStringList lookNames() {
  QStringList n;
  for (const Look& l : looks()) n << l.name;
  return n;
}
QStringList pacingNames() {
  QStringList n;
  for (const PacingProfile& p : pacings()) n << p.name;
  return n;
}
QStringList mixNames() {
  QStringList n;
  for (const MixProfile& m : mixes()) n << m.name;
  return n;
}
QStringList recipeNames() {
  QStringList n;
  for (const Recipe& r : recipes()) n << r.name;
  return n;
}

QJsonObject toJson(const Look& l) {
  const LookGuards& g = l.guards;
  QJsonArray tags;
  for (const QString& t : l.tags) tags.append(t);
  return QJsonObject{{q("name"), l.name}, {q("description"), l.description}, {q("tags"), tags}, {q("whenToUse"), QJsonArray::fromStringList(l.whenToUse)}, {q("avoidWhen"), QJsonArray::fromStringList(l.avoidWhen)}, {q("params"), l.params},
                     {q("grade"), sf::toJson(l.grade)},
                     {q("guards"), QJsonObject{{q("maxChromaP95"), g.maxChromaP95}, {q("maxGamutLimitPct"), g.maxGamutLimitPct}, {q("maxLiftMaster"), g.maxLiftMaster},
                                                {q("blacksCrushedIncrease"), g.blacksCrushedIncrease}, {q("highlightsClippedIncrease"), g.highlightsClippedIncrease},
                                                {q("skinHueShiftDeg"), g.skinHueShiftDeg}, {q("skinMinPct"), g.skinMinPct}, {q("exposureShiftStops"), g.exposureShiftStops},
                                                {q("skinHueBandDeg"), QJsonArray{g.skinBandMin, g.skinBandMax}}}}};
}

QJsonObject toJson(const PacingProfile& p) {
  return QJsonObject{{q("name"), p.name}, {q("description"), p.description}, {q("whenToUse"), QJsonArray::fromStringList(p.whenToUse)}, {q("avoidWhen"), QJsonArray::fromStringList(p.avoidWhen)}, {q("aslSec"), QJsonObject{{q("min"), p.aslMin}, {q("max"), p.aslMax}}}, {q("cutsOnBeatMinPct"), p.cutsOnBeatMinPct},
                     {q("hookMaxSec"), p.hookMaxSec}, {q("minShotSec"), p.minShotSec}, {q("maxShotSec"), p.maxShotSec}};
}

QJsonObject toJson(const MixProfile& m) {
  QJsonObject o{{q("name"), m.name}, {q("description"), m.description}, {q("whenToUse"), QJsonArray::fromStringList(m.whenToUse)}, {q("avoidWhen"), QJsonArray::fromStringList(m.avoidWhen)}, {q("targetLufs"), m.targetLufs}, {q("toleranceLu"), m.toleranceLu}, {q("truePeakCeilingDb"), m.truePeakCeilingDb},
                {q("duckMarginDb"), m.duckMarginDb}, {q("attackMs"), m.attackMs}, {q("releaseMs"), m.releaseMs}, {q("minSpeechMarginDb"), m.minSpeechMarginDb()}};
  if (m.altTargetLufs) o.insert(q("altTargetLufs"), *m.altTargetLufs);
  return o;
}

QJsonObject toJson(const AudioPreset& p) {
  QJsonArray chain;
  for (const AudioEffectSpec& e : p.chain) chain.append(QJsonObject{{q("type"), e.type}, {q("params"), e.params}});
  return QJsonObject{{q("id"), p.id}, {q("name"), p.name}, {q("category"), p.category}, {q("description"), p.description}, {q("whenToUse"), QJsonArray::fromStringList(p.whenToUse)},
                     {q("avoidWhen"), QJsonArray::fromStringList(p.avoidWhen)}, {q("chain"), chain}};
}

QJsonObject toJson(const Recipe& r) {
  QJsonArray stepsArr, verifyArr;
  for (const RecipeStep& s : r.steps) stepsArr.append(QJsonObject{{q("command"), s.command}, {q("args"), s.args}});
  for (const RecipeStep& v : r.verify) verifyArr.append(QJsonObject{{q("command"), v.command}, {q("args"), v.args}});
  return QJsonObject{{q("name"), r.name}, {q("description"), r.description}, {q("whenToUse"), QJsonArray::fromStringList(r.whenToUse)}, {q("avoidWhen"), QJsonArray::fromStringList(r.avoidWhen)},
                     {q("steps"), stepsArr}, {q("verify"), verifyArr}};
}

QJsonObject listJson(const QString& kind) {
  QJsonArray items;
  QStringList names;
  QJsonObject schema;
  if (kind == QLatin1String("look")) {
    for (const Look& l : looks()) { items.append(toJson(l)); names << l.name; }
    schema = lookSchema();
  } else if (kind == QLatin1String("pacing")) {
    for (const PacingProfile& p : pacings()) { items.append(toJson(p)); names << p.name; }
    schema = pacingSchema();
  } else if (kind == QLatin1String("recipe")) {
    for (const Recipe& r : recipes()) { items.append(toJson(r)); names << r.name; }
    schema = recipeSchema();
  } else {
    for (const MixProfile& m : mixes()) { items.append(toJson(m)); names << m.name; }
    schema = mixSchema();
  }
  return QJsonObject{{q("kind"), kind}, {q("names"), QJsonArray::fromStringList(names)}, {q("items"), items}, {q("schema"), schema}};
}

// ---------------------------------------------------------------- schemas

QJsonObject lookSchema() {
  return js::obj({{q("name"), nonEmptyStr()}, {q("description"), nonEmptyStr()}, {q("tags"), js::array(js::str())}, {q("whenToUse"), js::array(js::str())}, {q("avoidWhen"), js::array(js::str())}, {q("params"), primariesSchema()}, {q("guards"), guardsSchema()}}, {q("name"), q("description"), q("params")});
}

QJsonObject pacingSchema() {
  return js::obj({{q("name"), nonEmptyStr()},
                  {q("description"), nonEmptyStr()},
                  {q("whenToUse"), js::array(js::str())},
                  {q("avoidWhen"), js::array(js::str())},
                  {q("aslSec"), js::obj({{q("min"), js::number(QString(), 0.05, 600)}, {q("max"), js::number(QString(), 0.05, 600)}}, {q("min"), q("max")})},
                  {q("cutsOnBeatMinPct"), js::number(QStringLiteral("percent of cuts within +-2 frames of a beat; 0 = not checked"), 0, 100)},
                  {q("hookMaxSec"), js::number(QStringLiteral("the first cut comes within this"), 0.1, 600)},
                  {q("minShotSec"), js::number(QString(), 0.04, 60)},
                  {q("maxShotSec"), js::number(QString(), 0.1, 3600)}},
                 {q("name"), q("description"), q("aslSec"), q("cutsOnBeatMinPct"), q("hookMaxSec"), q("minShotSec"), q("maxShotSec")});
}

QJsonObject mixSchema() {
  return js::obj({{q("name"), nonEmptyStr()},
                  {q("description"), nonEmptyStr()},
                  {q("whenToUse"), js::array(js::str())},
                  {q("avoidWhen"), js::array(js::str())},
                  {q("targetLufs"), js::number(QStringLiteral("integrated loudness target"), -40, -5)},
                  {q("toleranceLu"), js::number(QString(), 0, 5)},
                  {q("truePeakCeilingDb"), js::number(QStringLiteral("dBTP"), -9, 0)},
                  {q("duckMarginDb"), js::number(QStringLiteral("speech over music while speech plays"), 1, 30)},
                  {q("attackMs"), js::number(QString(), 1, 2000)},
                  {q("releaseMs"), js::number(QString(), 1, 5000)},
                  {q("altTargetLufs"), js::number(QStringLiteral("an alternative standard's target"), -40, -5)}},
                 {q("name"), q("description"), q("targetLufs"), q("toleranceLu"), q("truePeakCeilingDb"), q("duckMarginDb"), q("attackMs"), q("releaseMs")});
}

QJsonObject audioPresetSchema() {
  QJsonObject effect = js::obj({{q("type"), js::choice({q("eq"), q("compressor"), q("limiter"), q("gate"), q("deesser"), q("reverb")})}, {q("params"), js::any(QStringLiteral("effect parameters by name"))}}, {q("type"), q("params")});
  QJsonObject chain = js::array(effect);
  chain.insert(q("minItems"), 1);
  return js::obj({{q("id"), nonEmptyStr()}, {q("name"), nonEmptyStr()}, {q("category"), js::choice({q("voice"), q("music"), q("fx"), q("master"), q("utility")})}, {q("description"), nonEmptyStr()},
                  {q("whenToUse"), nonEmptyItems(js::array(js::str()))}, {q("avoidWhen"), js::array(js::str())}, {q("chain"), chain}},
                 {q("id"), q("name"), q("category"), q("description"), q("whenToUse"), q("avoidWhen"), q("chain")});
}

QJsonObject recipeSchema() {
  QJsonObject step = js::obj({{q("command"), nonEmptyStr()}, {q("args"), js::any()}}, {q("command"), q("args")});
  return js::obj({{q("name"), nonEmptyStr()},
                  {q("description"), nonEmptyStr()},
                  {q("whenToUse"), nonEmptyItems(js::array(js::str()))},
                  {q("avoidWhen"), js::array(js::str())},
                  {q("steps"), nonEmptyItems(js::array(step))},
                  {q("verify"), js::array(step)}},
                 {q("name"), q("description"), q("whenToUse"), q("steps")});
}

QJsonObject resourceSchema(const QString& kind) {
  const QJsonObject item = kind == QLatin1String("look") ? lookSchema() : kind == QLatin1String("pacing") ? pacingSchema() : kind == QLatin1String("audio") ? audioPresetSchema() : kind == QLatin1String("recipe") ? recipeSchema() : mixSchema();
  return js::obj({{q("version"), js::integer(QString(), 1, 1)}, {q("kind"), js::choice({kind})}, {q("items"), nonEmptyItems(js::array(item))}}, {q("version"), q("kind"), q("items")});
}

QJsonObject specSchema() {
  QJsonObject s = js::obj({{q("look"), js::choice(lookNames(), QStringLiteral("A look from style.list kind=look; omit to leave the grade alone"))},
                           {q("intensity"), js::number(QStringLiteral("How much of the look, 0 (none) .. 1 (full); default 1"), 0, 1)},
                           {q("warmth"), js::number(QStringLiteral("Temperature offset added to the look, small: -0.2 (cooler) .. 0.2 (warmer)"), -kMaxWarmthOffset, kMaxWarmthOffset)},
                           {q("contrast"), js::number(QStringLiteral("Contrast offset: the look's contrast is multiplied by 1 + this, -0.2 .. 0.2"), -kMaxContrastOffset, kMaxContrastOffset)},
                           {q("saturation"), js::number(QStringLiteral("Saturation offset: multiplied by 1 + this, -0.2 .. 0.2"), -kMaxSaturationOffset, kMaxSaturationOffset)},
                           {q("pacing"), js::choice(pacingNames(), QStringLiteral("A pacing profile from style.list kind=pacing, checked with pacing.check"))},
                           {q("mix"), js::choice(mixNames(), QStringLiteral("A mix profile from style.list kind=mix, applied with mix.applyProfile"))}});
  s.insert(q("title"), q("SplitFrame style spec"));
  s.insert(q("description"),
           QStringLiteral("What a model fills in to choose a style. Every field is optional and bounded; the numbers behind each name come from the library, not from the model. look + intensity + warmth / contrast / saturation go to look.apply, "
                          "pacing to pacing.check, mix to mix.applyProfile."));
  return s;
}

// ---------------------------------------------------------------- validation

QStringList validateAudioPreset(const AudioPreset& p) {
  QStringList errors;
  const QString at = QStringLiteral("audio preset %1").arg(p.id);
  if (p.chain.empty()) errors << at + QStringLiteral(": empty chain");
  int n = 0;
  for (const AudioEffectSpec& e : p.chain) {
    const QString where = QStringLiteral("%1 effect %2 (%3)").arg(at).arg(n++).arg(e.type);
    const auto fx = audio::makeEffect(e.type, 48000.0);
    if (!fx) {
      errors << where + QStringLiteral(": unknown effect type");
      continue;
    }
    for (auto it = e.params.begin(); it != e.params.end(); ++it) {
      const audio::ParamDef* def = nullptr;
      for (const audio::ParamDef& d : fx->paramDefs())
        if (it.key() == QString::fromLatin1(d.name)) def = &d;
      if (!def) {
        errors << where + QStringLiteral(": unknown param \"%1\"").arg(it.key());
        continue;
      }
      if (it.value().isString()) {
        const QStringList choices = def->choices ? QString::fromLatin1(def->choices).split(QLatin1Char(',')) : QStringList{};
        if (!choices.contains(it.value().toString())) errors << where + QStringLiteral(": param %1 = \"%2\" is not one of [%3]").arg(it.key(), it.value().toString(), choices.join(QLatin1Char(',')));
      } else if (it.value().isDouble()) {
        const double v = it.value().toDouble();
        if (!std::isfinite(v) || v < def->min || v > def->max) errors << where + QStringLiteral(": param %1 = %2 outside %3 .. %4").arg(it.key()).arg(v).arg(def->min).arg(def->max);
      } else {
        errors << where + QStringLiteral(": param %1 must be a number or a choice name").arg(it.key());
      }
    }
  }
  return errors;
}

QStringList validateLibrary() {
  QStringList errors = lib().errors;
  static const QRegularExpression nameRx(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
  for (int k = 0; k < 5; ++k) {
    const QString file = q(kFiles[k]) + QStringLiteral(".json");
    const QJsonObject& raw = lib().raw[k];
    if (raw.isEmpty()) continue;
    if (const auto e = js::validate(raw, resourceSchema(q(kKinds[k])), file)) errors << *e;
    std::set<QString> seen;
    for (const QJsonValue& v : raw.value(q("items")).toArray()) {
      if (k == 3) {
        static const QRegularExpression idRx(QStringLiteral("^[a-z0-9]+(_[a-z0-9-]+)*$"));
        const QString id = v.toObject().value(q("id")).toString();
        if (!idRx.match(id).hasMatch()) errors << QStringLiteral("%1: \"%2\" is not a lowercase snake_case id").arg(file, id);
        if (!seen.insert(id).second) errors << QStringLiteral("%1: duplicate id \"%2\"").arg(file, id);
        continue;
      }
      const QString name = v.toObject().value(q("name")).toString();
      if (!nameRx.match(name).hasMatch()) errors << QStringLiteral("%1: \"%2\" is not a lowercase-hyphen name").arg(file, name);
      if (!seen.insert(name).second) errors << QStringLiteral("%1: duplicate name \"%2\"").arg(file, name);
    }
  }
  for (const Look& l : looks()) {
    if (l.grade == PrimaryGrade{}) errors << QStringLiteral("look %1: its params change nothing").arg(l.name);
    if (l.guards.skinBandMin >= l.guards.skinBandMax) errors << QStringLiteral("look %1: skinHueBandDeg is empty").arg(l.name);
    if (!(l.guards.skinBandMin <= 123.0 && 123.0 <= l.guards.skinBandMax)) errors << QStringLiteral("look %1: skinHueBandDeg does not contain the skin line (123)").arg(l.name);
    if (shadowLift(l.grade) > l.guards.maxLiftMaster + 1e-9) errors << QStringLiteral("look %1: its own shadow lift %2 breaks its maxLiftMaster %3").arg(l.name).arg(shadowLift(l.grade)).arg(l.guards.maxLiftMaster);
  }
  for (const AudioPreset& a : audioPresets()) errors << validateAudioPreset(a);
  for (const PacingProfile& p : pacings()) {
    if (p.aslMin > p.aslMax) errors << QStringLiteral("pacing %1: aslSec.min > aslSec.max").arg(p.name);
    if (p.minShotSec > p.aslMin) errors << QStringLiteral("pacing %1: minShotSec is above the shortest allowed average shot").arg(p.name);
    if (p.maxShotSec < p.aslMax) errors << QStringLiteral("pacing %1: maxShotSec is under the longest allowed average shot").arg(p.name);
    if (p.hookMaxSec > p.maxShotSec) errors << QStringLiteral("pacing %1: hookMaxSec is beyond maxShotSec").arg(p.name);
  }
  for (const MixProfile& m : mixes()) {
    if (m.altTargetLufs && std::fabs(*m.altTargetLufs - m.targetLufs) > 3.0) errors << QStringLiteral("mix %1: altTargetLufs is more than 3 LU from the target").arg(m.name);
    if (m.truePeakCeilingDb > -0.1) errors << QStringLiteral("mix %1: the true-peak ceiling leaves no headroom").arg(m.name);
  }
  for (const Recipe& r : recipes()) {
    if (r.steps.empty()) errors << QStringLiteral("recipe %1: steps is empty").arg(r.name);
    for (const RecipeStep& s : r.steps) {
      if (s.command == QLatin1String("look.apply")) {
        const QString lk = s.args.value(q("look")).toString();
        if (!lk.isEmpty() && !findLook(lk)) errors << QStringLiteral("recipe %1: unknown look \"%2\"").arg(r.name, lk);
      } else if (s.command == QLatin1String("mix.applyProfile")) {
        const QString mx = s.args.value(q("profile")).toString();
        if (!mx.isEmpty() && !findMix(mx)) errors << QStringLiteral("recipe %1: unknown mix \"%2\"").arg(r.name, mx);
      } else if (s.command == QLatin1String("effect.add")) {
        const QString ef = s.args.value(q("effectId")).toString();
        if (!ef.isEmpty() && !sf::render::FxRegistry::instance().find(ef)) errors << QStringLiteral("recipe %1: unknown effect \"%2\"").arg(r.name, ef);
      } else if (s.command == QLatin1String("transition.add")) {
        const QString tr = s.args.value(q("effectId")).toString();
        if (!tr.isEmpty() && !sf::render::FxRegistry::instance().find(tr)) errors << QStringLiteral("recipe %1: unknown transition \"%2\"").arg(r.name, tr);
      }
    }
    for (const RecipeStep& v : r.verify) {
      if (v.command == QLatin1String("pacing.check")) {
        const QString pc = v.args.value(q("profile")).toString();
        if (!pc.isEmpty() && !findPacing(pc)) errors << QStringLiteral("recipe %1 verify: unknown pacing \"%2\"").arg(r.name, pc);
      }
    }
  }
  return errors;
}

// ---------------------------------------------------------------- looks

namespace {

double clampD(double v, double lo, double hi) { return std::clamp(v, lo, hi); }
double lerp(double neutral, double v, double t) { return neutral + (v - neutral) * t; }

Rgb3 mapRgb(const Rgb3& a, const Rgb3& b, double (*f)(double, double)) { return {f(a.r, b.r), f(a.g, b.g), f(a.b, b.b)}; }
double add(double a, double b) { return a + b; }
double mul(double a, double b) { return a * b; }

void readTrio(const QJsonObject& o, double neutral, double& master, Rgb3& rgb) {
  master = numOr(o, "master", neutral);
  rgb = {numOr(o, "r", neutral), numOr(o, "g", neutral), numOr(o, "b", neutral)};
}

Rgb3 clampRgb(Rgb3 c, double lo, double hi) { return {clampD(c.r, lo, hi), clampD(c.g, lo, hi), clampD(c.b, lo, hi)}; }

PrimaryGrade clampGrade(PrimaryGrade g) {
  g.exposure = clampD(g.exposure, -4, 4);
  g.temperature = clampD(g.temperature, -1, 1);
  g.tint = clampD(g.tint, -1, 1);
  g.contrast = clampD(g.contrast, 0, 2);
  g.pivot = clampD(g.pivot, 0.001, 10);
  g.saturation = clampD(g.saturation, 0, 2);
  g.liftMaster = clampD(g.liftMaster, -0.5, 0.5);
  g.lift = clampRgb(g.lift, -0.5, 0.5);
  g.gammaMaster = clampD(g.gammaMaster, 0.2, 5);
  g.gamma = clampRgb(g.gamma, 0.2, 5);
  g.gainMaster = clampD(g.gainMaster, 0, 4);
  g.gain = clampRgb(g.gain, 0, 4);
  g.offset = clampRgb(g.offset, -1, 1);
  return g;
}

} // namespace

PrimaryGrade gradeFromJson(const QJsonObject& p) {
  PrimaryGrade g;
  g.exposure = numOr(p, "exposure", 0);
  g.temperature = numOr(p, "temperature", 0);
  g.tint = numOr(p, "tint", 0);
  g.contrast = numOr(p, "contrast", 1);
  g.pivot = numOr(p, "pivot", 0.18);
  g.saturation = numOr(p, "saturation", 1);
  readTrio(p.value(q("lift")).toObject(), 0, g.liftMaster, g.lift);
  readTrio(p.value(q("gamma")).toObject(), 1, g.gammaMaster, g.gamma);
  readTrio(p.value(q("gain")).toObject(), 1, g.gainMaster, g.gain);
  const QJsonObject off = p.value(q("offset")).toObject();
  g.offset = {numOr(off, "r", 0), numOr(off, "g", 0), numOr(off, "b", 0)};
  return clampGrade(g);
}

PrimaryGrade scaleGrade(const PrimaryGrade& g, double intensity) {
  const double t = clampD(intensity, 0, 1);
  PrimaryGrade o;
  o.exposure = lerp(0, g.exposure, t);
  o.temperature = lerp(0, g.temperature, t);
  o.tint = lerp(0, g.tint, t);
  o.contrast = lerp(1, g.contrast, t);
  o.pivot = g.pivot;
  o.saturation = lerp(1, g.saturation, t);
  o.liftMaster = lerp(0, g.liftMaster, t);
  o.lift = {lerp(0, g.lift.r, t), lerp(0, g.lift.g, t), lerp(0, g.lift.b, t)};
  o.gammaMaster = lerp(1, g.gammaMaster, t);
  o.gamma = {lerp(1, g.gamma.r, t), lerp(1, g.gamma.g, t), lerp(1, g.gamma.b, t)};
  o.gainMaster = lerp(1, g.gainMaster, t);
  o.gain = {lerp(1, g.gain.r, t), lerp(1, g.gain.g, t), lerp(1, g.gain.b, t)};
  o.offset = {lerp(0, g.offset.r, t), lerp(0, g.offset.g, t), lerp(0, g.offset.b, t)};
  return o;
}

PrimaryGrade layerGrade(const PrimaryGrade& base, const PrimaryGrade& d) {
  PrimaryGrade o = base;
  o.exposure = base.exposure + d.exposure;
  o.temperature = base.temperature + d.temperature;
  o.tint = base.tint + d.tint;
  o.contrast = base.contrast * d.contrast;
  o.saturation = base.saturation * d.saturation;
  o.liftMaster = base.liftMaster + d.liftMaster;
  o.lift = mapRgb(base.lift, d.lift, add);
  o.gammaMaster = base.gammaMaster * d.gammaMaster;
  o.gamma = mapRgb(base.gamma, d.gamma, mul);
  o.gainMaster = base.gainMaster * d.gainMaster;
  o.gain = mapRgb(base.gain, d.gain, mul);
  o.offset = mapRgb(base.offset, d.offset, add);
  return clampGrade(o);
}

PrimaryGrade applyOffsets(PrimaryGrade g, const SpecOffsets& o) {
  g.temperature += clampD(o.warmth, -kMaxWarmthOffset, kMaxWarmthOffset);
  g.contrast *= 1.0 + clampD(o.contrast, -kMaxContrastOffset, kMaxContrastOffset);
  g.saturation *= 1.0 + clampD(o.saturation, -kMaxSaturationOffset, kMaxSaturationOffset);
  return clampGrade(g);
}

PrimaryGrade proposeLook(const Look& look, double intensity, const SpecOffsets& offsets, const std::optional<PrimaryGrade>& existing) {
  const PrimaryGrade delta = applyOffsets(scaleGrade(look.grade, intensity), offsets);
  return existing ? layerGrade(*existing, delta) : clampGrade(delta);
}

double shadowLift(const PrimaryGrade& g) { return g.liftMaster + std::max({0.0, g.lift.r, g.lift.g, g.lift.b}); }

// ---------------------------------------------------------------- suggestions

namespace {
bool tagFound(const QString& tag, const QStringList& entries) {
  if (tag.isEmpty()) return false;
  static const QRegularExpression splitRx(QStringLiteral("[\\s,;:.]+"));
  for (const QString& e : entries) {
    const QString l = e.toLower();
    if (l == tag) return true;
    if (l.contains(QLatin1Char(' ')) && l.split(splitRx, Qt::SkipEmptyParts).contains(tag)) return true;
  }
  return false;
}
} // namespace

std::vector<Suggestion> rankByTags(const std::vector<Candidate>& candidates, const QStringList& queryTags) {
  std::vector<Suggestion> out;
  QStringList query;
  for (const QString& t : queryTags)
    if (!t.trimmed().isEmpty() && !query.contains(t.trimmed().toLower())) query << t.trimmed().toLower();
  for (const Candidate& c : candidates) {
    Suggestion s;
    s.id = c.id;
    for (const QString& t : query) {
      if (tagFound(t, c.positive)) s.matched << t;
      if (tagFound(t, c.avoid)) s.avoided << t;
    }
    s.score = static_cast<int>(s.matched.size()) - static_cast<int>(s.avoided.size());
    out.push_back(std::move(s));
  }
  std::stable_sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) { return a.score != b.score ? a.score > b.score : a.id < b.id; });
  return out;
}

// ---------------------------------------------------------------- LUT export

std::array<double, 3> gradeEncoded(const PrimaryGrade& g, render::Transfer transfer, const std::array<double, 3>& rgb) {
  if (render::gradeIsNeutral(g)) return {std::clamp(rgb[0], 0.0, 1.0), std::clamp(rgb[1], 0.0, 1.0), std::clamp(rgb[2], 0.0, 1.0)}; // the shader skips a neutral grade
  const render::GradeCoeffs c = render::gradeCoeffs(g);
  const render::Rgbd lin = render::decodeRgb(transfer, {rgb[0], rgb[1], rgb[2]});
  const render::Rgbd graded = render::applyGrade(c, lin);
  const render::Rgbd enc = render::encodeRgb(transfer, {std::max(graded[0], 0.0), std::max(graded[1], 0.0), std::max(graded[2], 0.0)});
  return {std::clamp(enc[0], 0.0, 1.0), std::clamp(enc[1], 0.0, 1.0), std::clamp(enc[2], 0.0, 1.0)};
}

QString lutCubeText(const PrimaryGrade& g, int size, const QString& inputSpace, const QString& title, QString* error) {
  const QString sp = inputSpace.trimmed().isEmpty() ? QStringLiteral("srgb") : inputSpace.trimmed().toLower();
  struct Named {
    const char* name;
    render::Transfer t;
    const char* label;
  };
  static const Named spaces[] = {{"srgb", render::Transfer::Srgb, "display-referred sRGB (also untagged / BT.709 SDR video as the editor shows it), Rec.709 primaries"},
                                 {"rec709", render::Transfer::Bt709, "Rec.709 BT.709-OETF encoded, Rec.709 primaries"},
                                 {"bt1886", render::Transfer::Bt1886, "display-referred Rec.709 / BT.1886 (gamma 2.4), Rec.709 primaries"},
                                 {"gamma22", render::Transfer::Gamma22, "display-referred gamma 2.2, Rec.709 primaries"},
                                 {"gamma28", render::Transfer::Gamma28, "display-referred gamma 2.8, Rec.709 primaries"},
                                 {"linear", render::Transfer::Linear, "scene-linear Rec.709 (1.0 = SDR white)"}};
  const Named* sel = nullptr;
  for (const Named& n : spaces)
    if (sp == QLatin1String(n.name)) sel = &n;
  if (!sel) {
    if (error) *error = QStringLiteral("Unknown inputSpace \"%1\": use srgb, rec709, bt1886, gamma22, gamma28 or linear.").arg(inputSpace);
    return {};
  }
  if (size < 2 || size > 129) {
    if (error) *error = QStringLiteral("size must be 2..129.");
    return {};
  }
  QString t = title;
  t.replace(QLatin1Char('"'), QLatin1Char('\'')).replace(QLatin1Char('\n'), QLatin1Char(' '));
  QString out;
  out.reserve(size * size * size * 28 + 256);
  out += QStringLiteral("TITLE \"%1 - expects %2 input, outputs the same encoding\"\n").arg(t, QString::fromLatin1(sel->label));
  out += QStringLiteral("LUT_3D_SIZE %1\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n").arg(size);
  const double d = static_cast<double>(size - 1);
  for (int b = 0; b < size; ++b)
    for (int gi = 0; gi < size; ++gi)
      for (int r = 0; r < size; ++r) {
        const std::array<double, 3> o = gradeEncoded(g, sel->t, {r / d, gi / d, b / d});
        out += QStringLiteral("%1 %2 %3\n").arg(o[0], 0, 'f', 6).arg(o[1], 0, 'f', 6).arg(o[2], 0, 'f', 6);
      }
  return out;
}

// ---------------------------------------------------------------- pacing

std::vector<PacingViolation> checkPacing(const PacingProfile& p, const tl::ShotStats& s, std::optional<double> beatSharePct, double fps) {
  std::vector<PacingViolation> v;
  if (s.shotCount == 0) return v;
  if (s.aslSec < p.aslMin)
    v.push_back({q("asl_too_fast"), s.aslSec, p.aslMin, std::nullopt, QStringLiteral("Shots average %1 s, under the %2 s this profile wants: slow the cutting (longer shots, fewer cuts).").arg(s.aslSec, 0, 'f', 2).arg(p.aslMin, 0, 'f', 2)});
  if (s.aslSec > p.aslMax)
    v.push_back({q("asl_too_slow"), s.aslSec, p.aslMax, std::nullopt, QStringLiteral("Shots average %1 s, over the %2 s this profile allows: tighten the cutting.").arg(s.aslSec, 0, 'f', 2).arg(p.aslMax, 0, 'f', 2)});
  const double hook = s.firstCutSec >= 0 ? s.firstCutSec : s.durationSec;
  if (hook > p.hookMaxSec)
    v.push_back({q("hook"), hook, p.hookMaxSec, std::nullopt,
                 s.firstCutSec >= 0 ? QStringLiteral("The first cut comes at %1 s, after the %2 s hook limit.").arg(hook, 0, 'f', 2).arg(p.hookMaxSec, 0, 'f', 2)
                                    : QStringLiteral("There is no cut in %1 s; this profile wants the first cut within %2 s.").arg(hook, 0, 'f', 2).arg(p.hookMaxSec, 0, 'f', 2)});
  if (p.cutsOnBeatMinPct > 0 && beatSharePct && !s.cuts.empty() && *beatSharePct < p.cutsOnBeatMinPct)
    v.push_back({q("beat_share"), *beatSharePct, p.cutsOnBeatMinPct, std::nullopt,
                 QStringLiteral("%1% of the cuts land on a beat, under the %2% this profile wants. edit.snapToBeats proposes the fix.").arg(*beatSharePct, 0, 'f', 0).arg(p.cutsOnBeatMinPct, 0, 'f', 0)});
  for (const tl::ShortShot& sh : s.shortest) {
    const double sec = fps > 0 ? static_cast<double>(sh.length) / fps : 0;
    if (sec < p.minShotSec)
      v.push_back({q("min_shot"), sec, p.minShotSec, sh.start, QStringLiteral("Shot %1 runs %2 s, under the %3 s minimum.").arg(sh.itemId).arg(sec, 0, 'f', 2).arg(p.minShotSec, 0, 'f', 2)});
  }
  if (s.maxSec > p.maxShotSec)
    v.push_back({q("max_shot"), s.maxSec, p.maxShotSec, std::nullopt, QStringLiteral("The longest shot runs %1 s, over the %2 s limit: cut away or trim it.").arg(s.maxSec, 0, 'f', 2).arg(p.maxShotSec, 0, 'f', 2)});
  return v;
}

} // namespace sf::engine::style
