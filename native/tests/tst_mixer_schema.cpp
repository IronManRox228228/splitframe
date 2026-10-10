#include "audio_testutil.h"

#include <QJsonDocument>
#include <QTest>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

namespace {

Mixer sampleMixer() {
  Mixer m;
  MixNode strip;
  strip.id = QStringLiteral("trk_a");
  strip.volume = 0.8;
  strip.pan = -0.25;
  strip.solo = true;
  strip.output = QStringLiteral("bus1");
  strip.sends = {{QStringLiteral("bus1"), 0.5, true}};
  MixInsert eq;
  eq.id = QStringLiteral("ins_1");
  eq.type = QStringLiteral("eq");
  eq.params = {{QStringLiteral("b2.on"), true}, {QStringLiteral("b2.type"), QStringLiteral("peak")}, {QStringLiteral("b2.gain"), 3.5}};
  eq.automation[QStringLiteral("gain")] = {{0, 0, Easing::Linear}, {30, 3, Easing::EaseIn}};
  eq.sidechain = QStringLiteral("trk_b");
  eq.state = QStringLiteral("AAEC");
  strip.inserts = {eq};
  strip.automation[QStringLiteral("volume")] = {{0, 1, Easing::Linear}, {60, 0, Easing::Hold}};
  m.strips = {strip};
  MixNode bus;
  bus.id = QStringLiteral("bus1");
  bus.name = QStringLiteral("Music");
  m.buses = {bus};
  m.master.volume = 0.9;
  return m;
}

} // namespace

class TstMixerSchema : public QObject {
  Q_OBJECT
private slots:
  void absentBlockLeavesJsonUntouched() {
    AudioFixture fx;
    fx.add(audioItem(fx.doc, 0, 30));
    const QByteArray before = timelineDocToJson(fx.doc);
    QVERIFY(!QJsonDocument::fromJson(before).object().contains(QStringLiteral("mixer")));
    const TimelineDoc back = parseTimelineDocJson(before);
    QVERIFY(!back.mixer);
    QCOMPARE(timelineDocToJson(back), before);
  }

  void roundTrips() {
    AudioFixture fx;
    fx.doc.mixer = sampleMixer();
    const QByteArray json = timelineDocToJson(fx.doc);
    const TimelineDoc back = parseTimelineDocJson(json);
    QVERIFY(back.mixer);
    QVERIFY(*back.mixer == *fx.doc.mixer);
    QCOMPARE(timelineDocToJson(back), json);
    QVERIFY(back == fx.doc);
  }

  void validatesValues() {
    AudioFixture fx;
    fx.doc.mixer = sampleMixer();
    QJsonObject o = toJson(fx.doc);
    QJsonObject mixer = o.value(QStringLiteral("mixer")).toObject();
    QJsonArray strips = mixer.value(QStringLiteral("strips")).toArray();
    QJsonObject s = strips[0].toObject();
    s.insert(QStringLiteral("pan"), 2.0);
    strips[0] = s;
    mixer.insert(QStringLiteral("strips"), strips);
    o.insert(QStringLiteral("mixer"), mixer);
    SF_THROWS(parseTimelineDocJson(QJsonDocument(o).toJson()), SchemaError);
  }

  void neutralMixerRendersIdentically() {
    AudioFixture fx;
    fx.sources->add(QStringLiteral("ast_music"), noiseSource(0.5f, 48000 * 5));
    fx.add(audioItem(fx.doc, 0, 60));
    const auto plain = fx.render(0, 60000);
    Mixer m;
    MixNode strip;
    strip.id = trackOfKind(fx.doc, TrackKind::Audio).id;
    m.strips = {strip};
    fx.doc.mixer = m;
    QVERIFY(fx.render(0, 60000) == plain);
  }

  void opAppliesAndInverts() {
    AudioFixture fx;
    fx.add(audioItem(fx.doc, 0, 30));
    const Op set = MixerSet{sampleMixer()};
    const ApplyResult r = applyOp(fx.doc, set);
    QVERIFY(r.doc.mixer && *r.doc.mixer == sampleMixer());
    QCOMPARE(r.inverse.size(), size_t(1));
    const ApplyResult back = applyOps(r.doc, r.inverse);
    QVERIFY(!back.doc.mixer);
    QVERIFY(back.doc == fx.doc);
    // replacing an existing block inverts to the old one
    Mixer other = sampleMixer();
    other.master.volume = 0.5;
    const ApplyResult r2 = applyOp(r.doc, MixerSet{other});
    QVERIFY(applyOps(r2.doc, r2.inverse).doc == r.doc);
    // clearing
    const ApplyResult r3 = applyOp(r.doc, MixerSet{std::nullopt});
    QVERIFY(!r3.doc.mixer);
    QVERIFY(applyOps(r3.doc, r3.inverse).doc == r.doc);

    // JSON form, alone and in a batch
    const Op parsed = parseOp(Rd(toJson(set)));
    QVERIFY(parsed == set);
    QCOMPARE(set.type(), QStringLiteral("mixer.set"));
    const Op batch = BatchOp{{set, MixerSet{std::nullopt}}};
    QVERIFY(!applyOp(fx.doc, batch).doc.mixer);
    QVERIFY(parseOp(Rd(toJson(MixerSet{std::nullopt}))) == Op(MixerSet{std::nullopt}));
  }
};

QTEST_GUILESS_MAIN(TstMixerSchema)
#include "tst_mixer_schema.moc"
