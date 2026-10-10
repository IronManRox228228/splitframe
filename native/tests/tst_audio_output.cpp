#include "audio/audio_output.h"
#include "audio/preview_audio.h"
#include "audio_testutil.h"

#include <QTest>

#include <thread>

using namespace sf;
using namespace sf::audio;
using namespace sf::test;

// Opens the real device for ~100 ms and plays only silence (gain 0 / empty timeline). Skipped without a device.
class TstAudioOutput : public QObject {
  Q_OBJECT
private slots:
  void deviceConsumesAndClockFollows() {
    AudioOutput out;
    QString err;
    if (!out.start(&err)) QSKIP(qPrintable(QStringLiteral("no audio device: %1").arg(err)));
    out.setGain(0.0f);
    AudioClockSource clock(&out);
    render::PlaybackClock pc(std::shared_ptr<render::ClockSource>(&clock, [](render::ClockSource*) {}));
    pc.setTimeline(30, 100000);
    std::vector<float> silence(480 * 2, 0.0f);
    auto pump = [&](int ms) {
      for (int i = 0; i < ms / 10; ++i) {
        out.write(silence.data(), 480);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    };
    pump(30); // prime
    clock.setAudioMaster(true);
    QVERIFY(clock.audioMaster());
    pc.play(1.0);
    const std::int64_t c0 = out.contentFrames();
    pump(100);
    const std::int64_t consumed = out.contentFrames() - c0;
    QVERIFY2(consumed > 2400 && consumed < 12000, qPrintable(QString::number(consumed)));
    // the clock advanced by what the device consumed (30 fps: 1600 samples per frame)
    const double frames = static_cast<double>(pc.poll().frame);
    QVERIFY2(std::fabs(frames - static_cast<double>(consumed) / 1600.0) < 3.0, qPrintable(QString::number(frames)));
    pc.pause();
    clock.setAudioMaster(false); // continuous: no jump to the steady base
    const auto f = pc.poll().frame;
    pump(20);
    QCOMPARE(pc.poll().frame, f);
    out.flush();
    out.stop();
  }

  void previewAudioSilentPlay() {
    PreviewAudio pa;
    QString err;
    if (!pa.openDevice(&err)) QSKIP("no audio device");
    auto doc = std::make_shared<TimelineDoc>(makeDoc()); // no clips: silence
    pa.setSession(doc, std::make_shared<MapProvider>());
    pa.play(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const std::int64_t pos = pa.positionSamples();
    QVERIFY2(pos > 1000 && pos < 20000, qPrintable(QString::number(pos)));
    QVERIFY(pa.clockSource()->audioMaster());
    pa.pause();
    QVERIFY(!pa.clockSource()->audioMaster());
  }

  void clockSourceWithoutDeviceIsSteady() {
    AudioClockSource clock;
    clock.setAudioMaster(true); // no output: refused
    QVERIFY(!clock.audioMaster());
    const auto a = clock.nowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    QVERIFY(clock.nowNs() > a);
  }
};

QTEST_GUILESS_MAIN(TstAudioOutput)
#include "tst_audio_output.moc"
