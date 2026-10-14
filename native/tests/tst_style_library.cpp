// The style library as data: every compiled-in resource parses and validates against its schema, the look maths (intensity, layering, offsets),
// the pacing comparison on synthetic timelines, and the style spec schema. Pure functions, no engine.

#include "engine/jschema.h"
#include "engine/style_library.h"
#include "test_helpers.h"

#include <QTest>

using namespace sf;
using namespace sf::engine;
using namespace sf::engine::style;
using namespace sf::test;

namespace {

QString Q(const char* s) { return QString::fromLatin1(s); }

tl::ShotStats statsOf(const std::vector<Frame>& lengths, double fps = 30.0) {
  TimelineDoc d = makeDoc();
  std::vector<Item> items;
  Frame at = 0;
  for (Frame len : lengths) {
    items.push_back(videoItem(d, at, len));
    at += len;
  }
  d = docWith(d, items);
  return tl::computeStats(tl::shotsOnTrack(d, trackOfKind(d, TrackKind::Video).id), fps);
}

bool sameGrade(const PrimaryGrade& a, const PrimaryGrade& b) {
  const auto near = [](double x, double y) { return std::fabs(x - y) < 1e-12; };
  return near(a.exposure, b.exposure) && near(a.temperature, b.temperature) && near(a.tint, b.tint) && near(a.contrast, b.contrast) && near(a.saturation, b.saturation) && near(a.liftMaster, b.liftMaster) &&
         near(a.lift.r, b.lift.r) && near(a.lift.g, b.lift.g) && near(a.lift.b, b.lift.b) && near(a.gammaMaster, b.gammaMaster) && near(a.gain.r, b.gain.r) && near(a.gain.b, b.gain.b);
}

QStringList rules(const std::vector<PacingViolation>& v) {
  QStringList r;
  for (const PacingViolation& x : v) r << x.rule;
  return r;
}

} // namespace

class TstStyleLibrary : public QObject {
  Q_OBJECT
private slots:
  void everyResourceParsesAndValidates() {
    QCOMPARE(validateLibrary(), QStringList());
    QCOMPARE(looks().size(), size_t{8});
    QCOMPARE(pacings().size(), size_t{5});
    QCOMPARE(mixes().size(), size_t{4});
    for (const char* n : {"neutral-clean", "warm-documentary", "teal-orange", "low-key-cool", "bleach-bypass", "pastel", "golden-hour", "night-cool"}) QVERIFY2(findLook(Q(n)), n);
    for (const char* n : {"music-video", "trailer", "documentary", "interview", "social-short"}) QVERIFY2(findPacing(Q(n)), n);
    for (const char* n : {"podcast", "streaming", "cinematic", "social"}) QVERIFY2(findMix(Q(n)), n);
    // each item, as the commands show it, satisfies its own schema
    for (const Look& l : looks()) {
      QVERIFY(l.grade != PrimaryGrade{});
      const QJsonObject raw{{Q("name"), l.name}, {Q("description"), l.description}, {Q("params"), l.params}, {Q("guards"), l.guardsJson}};
      const auto err = js::validate(raw, lookSchema());
      QVERIFY2(!err, qPrintable(l.name + Q(": ") + err.value_or(QString())));
      QVERIFY(l.guards.skinBandMin < 123 && 123 < l.guards.skinBandMax);
    }
    for (const PacingProfile& p : pacings()) QVERIFY2(!js::validate(toJson(p), pacingSchema()), qPrintable(p.name));
    for (const MixProfile& m : mixes()) {
      QJsonObject o = toJson(m);
      o.remove(Q("minSpeechMarginDb")); // derived, not stored
      QVERIFY2(!js::validate(o, mixSchema()), qPrintable(m.name));
    }
    // the schema rejects what it should
    QVERIFY(js::validate(QJsonObject{{Q("name"), Q("x")}, {Q("description"), Q("y")}, {Q("params"), QJsonObject{{Q("exposure"), 9}}}}, lookSchema()));
    QVERIFY(js::validate(QJsonObject{{Q("name"), Q("x")}, {Q("description"), Q("y")}, {Q("params"), QJsonObject{{Q("bogus"), 1}}}}, lookSchema()));
    QVERIFY(js::validate(QJsonObject{{Q("name"), Q("x")}}, lookSchema()));
    QVERIFY(js::validate(QJsonObject{{Q("name"), Q("x")}, {Q("description"), Q("y")}, {Q("params"), QJsonObject{}}, {Q("guards"), QJsonObject{{Q("skinHueBandDeg"), QJsonArray{100}}}}}, lookSchema()));
    // the lists carry their schemas
    for (const char* k : {"look", "pacing", "mix"}) {
      const QJsonObject l = listJson(Q(k));
      QVERIFY(l.value(Q("schema")).toObject().contains(Q("properties")));
      QVERIFY(l.value(Q("items")).toArray().size() >= 4);
    }
  }

  void looksStayInsideTheGradeBounds() {
    // looks use only what primary correction supports, within what item.color.set accepts, at every intensity and extreme offset
    for (const Look& l : looks())
      for (double i : {0.0, 0.5, 1.0})
        for (double w : {-0.2, 0.0, 0.2}) {
          const PrimaryGrade g = proposeLook(l, i, {w, w, w}, std::nullopt);
          QVERIFY(std::fabs(g.exposure) <= 4 && std::fabs(g.temperature) <= 1 && g.contrast >= 0 && g.contrast <= 2 && g.saturation >= 0 && g.saturation <= 2);
          QVERIFY(g.gain.r >= 0 && g.gain.r <= 4 && g.gamma.b >= 0.2);
          QVERIFY2(toJson(g).contains(Q("pivot")) == false, "pivot stays at its default");
        }
    // lift of every look stays under its own cap
    for (const Look& l : looks()) QVERIFY2(shadowLift(l.grade) <= l.guards.maxLiftMaster, qPrintable(l.name));
  }

  void intensityScalesFromNeutral() {
    const Look& warm = *findLook(Q("warm-documentary"));
    QCOMPARE(proposeLook(warm, 0.0, {}, std::nullopt), PrimaryGrade{});
    const PrimaryGrade full = proposeLook(warm, 1.0, {}, std::nullopt), half = proposeLook(warm, 0.5, {}, std::nullopt);
    QVERIFY(sameGrade(full, warm.grade));
    QVERIFY(std::fabs(half.temperature - full.temperature / 2) < 1e-12);
    QVERIFY(std::fabs(half.contrast - (1 + (full.contrast - 1) / 2)) < 1e-12);
    QVERIFY(std::fabs(half.saturation - (1 + (full.saturation - 1) / 2)) < 1e-12);
    QVERIFY(std::fabs(half.liftMaster - full.liftMaster / 2) < 1e-12);
    QVERIFY(full.temperature > 0);                                  // warm
    QVERIFY(proposeLook(*findLook(Q("night-cool")), 1.0, {}, std::nullopt).temperature < 0); // cool
    QVERIFY(proposeLook(*findLook(Q("night-cool")), 1.0, {}, std::nullopt).exposure < 0);
    QVERIFY(proposeLook(*findLook(Q("bleach-bypass")), 1.0, {}, std::nullopt).saturation < 0.7);
    // intensity outside 0..1 is clamped
    QCOMPARE(proposeLook(warm, 3.0, {}, std::nullopt), full);
    QCOMPARE(proposeLook(warm, -1.0, {}, std::nullopt), PrimaryGrade{});
  }

  void offsetsAreSmallAndBounded() {
    const Look& l = *findLook(Q("neutral-clean"));
    const PrimaryGrade base = proposeLook(l, 1.0, {}, std::nullopt);
    const PrimaryGrade warmer = proposeLook(l, 1.0, {0.1, 0.0, 0.0}, std::nullopt);
    QVERIFY(std::fabs(warmer.temperature - base.temperature - 0.1) < 1e-12);
    QCOMPARE(warmer.contrast, base.contrast);
    const PrimaryGrade punchy = proposeLook(l, 1.0, {0.0, 0.1, -0.1}, std::nullopt);
    QVERIFY(std::fabs(punchy.contrast - base.contrast * 1.1) < 1e-12);
    QVERIFY(std::fabs(punchy.saturation - base.saturation * 0.9) < 1e-12);
    // far outside the allowed range is clipped to it
    QVERIFY(std::fabs(proposeLook(l, 1.0, {5.0, 0.0, 0.0}, std::nullopt).temperature - (base.temperature + kMaxWarmthOffset)) < 1e-12);
  }

  void looksLayerOnTheExistingGrade() {
    PrimaryGrade existing;
    existing.exposure = 0.5;
    existing.temperature = 0.1;
    existing.contrast = 1.1;
    existing.saturation = 0.9;
    existing.lift.b = 0.01;
    existing.gain.r = 1.1;
    const Look& teal = *findLook(Q("teal-orange"));
    const PrimaryGrade layered = proposeLook(teal, 1.0, {}, existing);
    QVERIFY(std::fabs(layered.exposure - (0.5 + teal.grade.exposure)) < 1e-12);
    QVERIFY(std::fabs(layered.temperature - (0.1 + teal.grade.temperature)) < 1e-12);
    QVERIFY(std::fabs(layered.contrast - 1.1 * teal.grade.contrast) < 1e-12);
    QVERIFY(std::fabs(layered.saturation - 0.9 * teal.grade.saturation) < 1e-12);
    QVERIFY(std::fabs(layered.lift.b - (0.01 + teal.grade.lift.b)) < 1e-12);
    QVERIFY(std::fabs(layered.gain.r - 1.1 * teal.grade.gain.r) < 1e-12);
    QCOMPARE(layered.pivot, existing.pivot);
    // layering on nothing is the look; replacing ignores the existing grade
    QCOMPARE(proposeLook(teal, 1.0, {}, PrimaryGrade{}), proposeLook(teal, 1.0, {}, std::nullopt));
    // stacking a look twice cannot leave the bounds
    PrimaryGrade g = existing;
    for (int i = 0; i < 20; ++i) g = proposeLook(*findLook(Q("bleach-bypass")), 1.0, {0.2, 0.2, 0.2}, g);
    QVERIFY(g.contrast <= 2 && g.saturation >= 0 && g.temperature <= 1);
    // gradeFromJson round trip
    QCOMPARE(gradeFromJson(toJson(layered)), layered);
    QCOMPARE(gradeFromJson(QJsonObject{}), PrimaryGrade{});
  }

  void pacingAgainstProfiles() {
    // 12 shots of 1 s: a fast cutter
    const tl::ShotStats fast = statsOf(std::vector<Frame>(12, 30));
    QCOMPARE(rules(checkPacing(*findPacing(Q("documentary")), fast, std::nullopt, 30.0)), QStringList{Q("asl_too_fast")});
    QCOMPARE(rules(checkPacing(*findPacing(Q("music-video")), fast, 80.0, 30.0)), QStringList{Q("asl_too_fast")}); // 1.0 s < 1.2 s
    QVERIFY(checkPacing(*findPacing(Q("social-short")), fast, 50.0, 30.0).empty());
    // the share of cuts on the beat is only checked for music-driven profiles and only when a grid exists
    QCOMPARE(rules(checkPacing(*findPacing(Q("social-short")), fast, 10.0, 30.0)), QStringList{Q("beat_share")});
    QVERIFY(checkPacing(*findPacing(Q("social-short")), fast, std::nullopt, 30.0).empty());
    QVERIFY(checkPacing(*findPacing(Q("documentary")), statsOf(std::vector<Frame>(6, 150)), 0.0, 30.0).empty()); // 5 s shots, no beat share wanted
    const std::vector<PacingViolation> bs = checkPacing(*findPacing(Q("social-short")), fast, 10.0, 30.0);
    QCOMPARE(bs[0].measured, 10.0);
    QCOMPARE(bs[0].limit, 30.0);

    // two long shots: too slow for a trailer, a late hook, a shot over the limit
    const tl::ShotStats slow = statsOf({300, 450});
    const std::vector<PacingViolation> tr = checkPacing(*findPacing(Q("trailer")), slow, std::nullopt, 30.0);
    QCOMPARE(rules(tr), (QStringList{Q("asl_too_slow"), Q("hook"), Q("max_shot")}));
    QCOMPARE(tr[0].measured, 12.5);
    QCOMPARE(tr[0].limit, 2.5);
    QCOMPARE(tr[1].measured, 10.0); // the first cut
    QCOMPARE(tr[2].measured, 15.0);
    QVERIFY(!tr[2].frame.has_value());

    // a 3 frame shot among normal ones
    const std::vector<PacingViolation> flash = checkPacing(*findPacing(Q("documentary")), statsOf({150, 3, 150, 150}), std::nullopt, 30.0);
    QCOMPARE(rules(flash).count(Q("min_shot")), 1);
    const PacingViolation* ms = nullptr;
    for (const PacingViolation& x : flash)
      if (x.rule == Q("min_shot")) ms = &x;
    QVERIFY(ms && ms->frame.has_value());
    QCOMPARE(*ms->frame, Frame{150});
    QVERIFY(std::fabs(ms->measured - 0.1) < 1e-9);

    // no cut at all: the hook is the whole length
    const std::vector<PacingViolation> one = checkPacing(*findPacing(Q("interview")), statsOf({300}), std::nullopt, 30.0);
    QVERIFY(rules(one).contains(Q("hook")));
    QCOMPARE(one[0].measured, 10.0);
    QVERIFY(checkPacing(*findPacing(Q("interview")), tl::ShotStats{}, std::nullopt, 30.0).empty());
  }

  void mixProfilesCarryTheirTargets() {
    const MixProfile& pod = *findMix(Q("podcast"));
    QCOMPARE(pod.targetLufs, -16.0);
    QCOMPARE(findMix(Q("streaming"))->targetLufs, -14.0);
    QCOMPARE(findMix(Q("social"))->targetLufs, -14.0);
    QCOMPARE(findMix(Q("cinematic"))->targetLufs, -23.0);
    QVERIFY(findMix(Q("cinematic"))->altTargetLufs.has_value());
    QCOMPARE(*findMix(Q("cinematic"))->altTargetLufs, -24.0);
    for (const MixProfile& m : mixes()) {
      QVERIFY2(m.truePeakCeilingDb < 0 && m.attackMs > 0 && m.releaseMs > m.attackMs && m.duckMarginDb > 3, qPrintable(m.name));
      QVERIFY(m.minSpeechMarginDb() <= m.duckMarginDb);
    }
    QVERIFY(findMix(Q("social"))->releaseMs < findMix(Q("podcast"))->releaseMs); // snappier ducks for social
  }

  void styleSpecSchema() {
    const QJsonObject schema = specSchema();
    const QJsonObject props = schema.value(Q("properties")).toObject();
    for (const char* k : {"look", "intensity", "warmth", "contrast", "saturation", "pacing", "mix"}) QVERIFY2(props.contains(Q(k)), k);
    QCOMPARE(props.value(Q("look")).toObject().value(Q("enum")).toArray().size(), 8);
    QCOMPARE(props.value(Q("pacing")).toObject().value(Q("enum")).toArray().size(), 5);
    QCOMPARE(props.value(Q("mix")).toObject().value(Q("enum")).toArray().size(), 4);
    QCOMPARE(props.value(Q("warmth")).toObject().value(Q("maximum")).toDouble(), 0.2);
    QCOMPARE(props.value(Q("intensity")).toObject().value(Q("minimum")).toDouble(), 0.0);
    QCOMPARE(schema.value(Q("additionalProperties")).toBool(true), false);
    QVERIFY(!js::validate(QJsonObject{{Q("look"), Q("teal-orange")}, {Q("intensity"), 0.6}, {Q("warmth"), 0.05}, {Q("contrast"), -0.1}, {Q("saturation"), 0.0}, {Q("pacing"), Q("trailer")}, {Q("mix"), Q("cinematic")}}, schema));
    QVERIFY(!js::validate(QJsonObject{}, schema));
    QVERIFY(js::validate(QJsonObject{{Q("look"), Q("sepia-dream")}}, schema));                  // not in the library
    QVERIFY(js::validate(QJsonObject{{Q("intensity"), 1.5}}, schema));                           // out of range
    QVERIFY(js::validate(QJsonObject{{Q("warmth"), 0.5}}, schema));                              // a big swing is not allowed
    QVERIFY(js::validate(QJsonObject{{Q("exposure"), 1.0}}, schema));                            // the spec is not a place for raw parameters
    QVERIFY(js::validate(QJsonObject{{Q("mix"), Q("loudest")}}, schema));
  }
};

QTEST_APPLESS_MAIN(TstStyleLibrary)
#include "tst_style_library.moc"
