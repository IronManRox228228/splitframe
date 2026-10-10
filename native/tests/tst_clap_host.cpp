#include "audio/clap_host.h"
#include "audio_testutil.h"

#include <QDir>
#include <QFileInfo>
#include <QTest>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

// CMake builds the test plugin (tests/clap_test_gain.cpp) and passes its path in SF_TEST_CLAP.
class TstClapHost : public QObject {
  Q_OBJECT
  QString plugin_ = QStringLiteral(SF_TEST_CLAP);
  QString id_ = QStringLiteral("org.splitframe.test.gain");

private slots:
  void scansAFolder() {
    const auto found = scanClapPlugins({QFileInfo(plugin_).absolutePath()});
    QVERIFY(!found.empty());
    bool ok = false;
    for (const auto& p : found)
      if (p.id == id_) {
        ok = true;
        QCOMPARE(p.name, QStringLiteral("Test Gain"));
        QVERIFY(p.features.contains(QStringLiteral("audio-effect")));
      }
    QVERIFY(ok);
    QVERIFY(scanClapPlugins({QStringLiteral("C:/definitely/not/here")}).empty());
    QVERIFY(!defaultClapSearchPaths().isEmpty());
  }

  void processesAudioAndParams() {
    QString err;
    auto fx = loadClapPlugin(plugin_, id_, 48000, &err);
    QVERIFY2(fx, qPrintable(err));
    QCOMPARE(fx->paramDefs().size(), size_t(1));
    auto sig = makeSignal(1000, [](std::int64_t i, int) { return sineAt(440, 0.5, i); });
    auto out = sig;
    fx->process(out.data(), 1000, nullptr);
    for (size_t i = 0; i < out.size(); ++i) QCOMPARE(out[i], sig[i]); // gain 1
    QVERIFY(fx->setParam(QStringLiteral("Gain"), 0.25));
    out = sig;
    fx->process(out.data(), 1000, nullptr);
    for (size_t i = 0; i < out.size(); ++i) QVERIFY(std::fabs(out[i] - sig[i] * 0.25f) < 1e-7f);
    QVERIFY(!fx->setParam(QStringLiteral("nope"), 1));
    // longer than the internal chunk of the host
    auto big = makeSignal(10000, [](std::int64_t, int) { return 1.0f; });
    fx->process(big.data(), 10000, nullptr);
    QCOMPARE(big[9999 * 2 + 1], 0.25f);
  }

  void savesAndRestoresState() {
    auto a = loadClapPlugin(plugin_, id_, 48000);
    QVERIFY(a);
    a->setParam(QStringLiteral("Gain"), 2.0);
    std::vector<float> tmp(64, 0.0f);
    a->process(tmp.data(), 32, nullptr); // delivers the parameter event
    const QByteArray state = a->saveState();
    QVERIFY(!state.isEmpty());
    auto b = loadClapPlugin(plugin_, id_, 48000);
    QVERIFY(b->loadState(state));
    std::vector<float> one(64, 1.0f);
    b->process(one.data(), 32, nullptr);
    QCOMPARE(one[0], 2.0f);
    QVERIFY(!b->loadState(QByteArray()));
  }

  void failsCleanly() {
    QString err;
    QVERIFY(!loadClapPlugin(QStringLiteral("C:/nope.clap"), id_, 48000, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!loadClapPlugin(plugin_, QStringLiteral("no.such.id"), 48000, &err));
  }

  void runsAsMixerInsertWithSavedState() {
    AudioFixture fx;
    fx.sources->add(QStringLiteral("ast_music"), dcSource(0.5f, 48000 * 3));
    fx.add(audioItem(fx.doc, 0, 60));
    auto host = loadClapPlugin(plugin_, id_, 48000);
    host->setParam(QStringLiteral("Gain"), 0.5);
    std::vector<float> tmp(64, 0.0f);
    host->process(tmp.data(), 32, nullptr);
    const QString b64 = QString::fromLatin1(host->saveState().toBase64());

    Mixer m;
    MixInsert ins;
    ins.id = QStringLiteral("ins_clap");
    ins.type = QStringLiteral("clap");
    ins.params = {{QStringLiteral("pluginPath"), plugin_}, {QStringLiteral("pluginId"), id_}};
    ins.state = b64;
    MixNode strip;
    strip.id = trackOfKind(fx.doc, TrackKind::Audio).id;
    strip.inserts = {ins};
    m.strips = {strip};
    fx.doc.mixer = m;
    const auto out = fx.render(100, 100);
    QCOMPARE(out[0], 0.25f);
    QCOMPARE(out[199], 0.25f);

    // the state round-trips through the document JSON
    const TimelineDoc back = parseTimelineDocJson(timelineDocToJson(fx.doc));
    QCOMPARE(*back.mixer->strips[0].inserts[0].state, b64);

    // the engine can report the live plugin state for saving
    AudioEngine eng(std::make_shared<const TimelineDoc>(fx.doc), fx.sources);
    QVERIFY(eng.pluginStates().count(QStringLiteral("ins_clap")) == 1);
  }
};

QTEST_GUILESS_MAIN(TstClapHost)
#include "tst_clap_host.moc"
