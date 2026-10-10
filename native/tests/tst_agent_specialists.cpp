// Unit tests for the three specialist agents (Editor, Colourist, Audio) in M5b.

#include "agent/specialist.h"
#include "engine/engine.h"

#include <QTest>

using namespace sf::agent;
using namespace sf::engine;

class TstAgentSpecialists : public QObject {
  Q_OBJECT

  std::unique_ptr<Engine> engine_;
  std::unique_ptr<PeerBus> bus_;

private slots:
  void initTestCase() {}

  void init() {
    engine_ = std::make_unique<Engine>();
    bus_ = std::make_unique<PeerBus>();
    engine_->call(QStringLiteral("project.new"), QJsonObject{
      {QStringLiteral("name"), QStringLiteral("AgentFixture")},
      {QStringLiteral("width"), 1920},
      {QStringLiteral("height"), 1080},
      {QStringLiteral("fps"), 30}
    });
  }

  void cleanup() {
    bus_.reset();
    engine_.reset();
  }

  // ---- domain ownership and identity ----
  void domainOwnership() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get());
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), bus_.get());
    Specialist audio(SpecialistKind::Audio, engine_.get(), bus_.get());

    QCOMPARE(editor.name(), QStringLiteral("editor"));
    QCOMPARE(editor.domain(), QStringLiteral("editor"));
    QCOMPARE(editor.slotId(), 0);

    QCOMPARE(colourist.name(), QStringLiteral("colourist"));
    QCOMPARE(colourist.domain(), QStringLiteral("colour"));
    QCOMPARE(colourist.slotId(), 1);

    QCOMPARE(audio.name(), QStringLiteral("audio"));
    QCOMPARE(audio.domain(), QStringLiteral("audio"));
    QCOMPARE(audio.slotId(), 2);

    // Contexts are separate
    editor.addMessage(ChatMessage{.role = QStringLiteral("user"), .content = QStringLiteral("Hello editor")});
    QCOMPARE(editor.history().size(), static_cast<size_t>(1));
    QCOMPARE(colourist.history().size(), static_cast<size_t>(0));
    QCOMPARE(audio.history().size(), static_cast<size_t>(0));
  }

  // ---- scoped views ----
  void scopedViews() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get());
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), bus_.get());
    Specialist audio(SpecialistKind::Audio, engine_.get(), bus_.get());

    QJsonObject edView = editor.scopedViewJson();
    QVERIFY(edView.contains(QStringLiteral("canvas")));
    QCOMPARE(edView.value(QStringLiteral("canvas")).toObject().value(QStringLiteral("width")).toInt(), 1920);

    QJsonObject colView = colourist.scopedViewJson();
    QVERIFY(colView.contains(QStringLiteral("colorManagement")));

    QJsonObject audView = audio.scopedViewJson();
    QVERIFY(audView.contains(QStringLiteral("mixer")));
  }

  // ---- strict domain enforcement ----
  void domainEnforcement() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get());
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), bus_.get());

    // 1. Editor can execute an "engine" command
    Result rEngine = editor.executeCommand(QStringLiteral("timeline.get"));
    QVERIFY(rEngine.ok);

    // 2. Editor attempting to execute an "audio" command directly is rejected!
    Result rAudio = editor.executeCommand(QStringLiteral("mixer.get"));
    QVERIFY(!rAudio.ok);
    QCOMPARE(rAudio.error.code, QStringLiteral("domain_forbidden"));
    QVERIFY(rAudio.error.message.contains(QStringLiteral("Peer Bus")));

    // 3. Colourist attempting to execute an "editor" command directly is rejected!
    Result rEdit = colourist.executeCommand(QStringLiteral("track.add"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("video")}});
    QVERIFY(!rEdit.ok);
    QCOMPARE(rEdit.error.code, QStringLiteral("domain_forbidden"));
    QVERIFY(rEdit.error.message.contains(QStringLiteral("Video Editor")));
  }

  // ---- peer request delegation ----
  void peerRequestDelegation() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get());
    Specialist audio(SpecialistKind::Audio, engine_.get(), bus_.get());

    bus_->setDefaultHandler(SpecialistKind::Audio, [&audio](const PeerRequest& req) {
      return audio.handlePeerRequest(req);
    });

    PeerResponse resp = editor.sendPeerRequest(
      SpecialistKind::Audio,
      QStringLiteral("duck_music"),
      QJsonObject{{QStringLiteral("volume"), 0.3}},
      {},
      QStringLiteral("dialogue at 0:10")
    );

    QCOMPARE(resp.status, QStringLiteral("done"));
    QCOMPARE(resp.outputs.value(QStringLiteral("duckedVolume")).toDouble(), 0.3);

    auto log = bus_->teamThread();
    QCOMPARE(static_cast<int>(log.size()), 1);
    QCOMPARE(log[0].from, SpecialistKind::Editor);
    QCOMPARE(log[0].to, SpecialistKind::Audio);
  }
};

QTEST_MAIN(TstAgentSpecialists)
#include "tst_agent_specialists.moc"
