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

    // 2. Editor may READ another domain's state (read tools are shared), so mixer.get is allowed.
    Result rRead = editor.executeCommand(QStringLiteral("mixer.get"));
    QVERIFY(rRead.ok);

    // 3. Editor attempting to MUTATE an "audio" command directly is rejected!
    Result rAudio = editor.executeCommand(QStringLiteral("audio.duck"));
    QVERIFY(!rAudio.ok);
    QCOMPARE(rAudio.error.code, QStringLiteral("domain_forbidden"));
    QVERIFY(rAudio.error.message.contains(QStringLiteral("Peer Bus")));

    // 4. Colourist attempting to execute an "editor" command directly is rejected!
    Result rEdit = colourist.executeCommand(QStringLiteral("track.add"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("video")}});
    QVERIFY(!rEdit.ok);
    QCOMPARE(rEdit.error.code, QStringLiteral("domain_forbidden"));
    QVERIFY(rEdit.error.message.contains(QStringLiteral("Video Editor")));

    // 5. generate.* and comfy.* overrides are open to every specialist even when they mutate (Part C).
    engine_->add(sf::engine::CommandSpec{
      .name = QStringLiteral("generate.test"),
      .alias = QString(),
      .description = QStringLiteral("Fake cross-domain generator allowed for every specialist."),
      .domain = QStringLiteral("genhub"),
      .mutates = true,
      .needsSession = false,
      .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
      .handler = [](sf::engine::CallContext&, const QJsonObject&) { return Result::success(); }
    });
    engine_->add(sf::engine::CommandSpec{
      .name = QStringLiteral("genhub.set"),
      .alias = QString(),
      .description = QStringLiteral("Fake cross-domain mutation NOT allowed via the domain rule."),
      .domain = QStringLiteral("genhub"),
      .mutates = true,
      .needsSession = false,
      .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
      .handler = [](sf::engine::CallContext&, const QJsonObject&) { return Result::success(); }
    });
    engine_->add(sf::engine::CommandSpec{
      .name = QStringLiteral("comfy.thing"),
      .alias = QString(),
      .description = QStringLiteral("Fake comfy-domain mutation allowed via the comfy override."),
      .domain = QStringLiteral("comfy"),
      .mutates = true,
      .needsSession = false,
      .inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}},
      .handler = [](sf::engine::CallContext&, const QJsonObject&) { return Result::success(); }
    });

    const sf::engine::CommandSpec* genSpec = engine_->find(QStringLiteral("generate.test"));
    QVERIFY(genSpec);
    QVERIFY(editor.allowsCommand(*genSpec));
    QVERIFY(editor.executeCommand(QStringLiteral("generate.test")).ok);

    const sf::engine::CommandSpec* comfySpec = engine_->find(QStringLiteral("comfy.thing"));
    QVERIFY(comfySpec);
    QVERIFY(editor.allowsCommand(*comfySpec));
    QVERIFY(editor.executeCommand(QStringLiteral("comfy.thing")).ok);

    const sf::engine::CommandSpec* genhubSpec = engine_->find(QStringLiteral("genhub.set"));
    QVERIFY(genhubSpec);
    QVERIFY(!editor.allowsCommand(*genhubSpec));
    Result rHub = editor.executeCommand(QStringLiteral("genhub.set"));
    QVERIFY(!rHub.ok);
    QCOMPARE(rHub.error.code, QStringLiteral("domain_forbidden"));
  }

  // ---- peer request delegation (real command routing, no canned replies) ----
  void peerRequestDelegation() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get());
    Specialist colourist(SpecialistKind::Colourist, engine_.get(), bus_.get());
    Specialist audio(SpecialistKind::Audio, engine_.get(), bus_.get());

    bus_->setDefaultHandler(SpecialistKind::Colourist, [&colourist](const PeerRequest& req) {
      return colourist.handlePeerRequest(req);
    });
    bus_->setDefaultHandler(SpecialistKind::Audio, [&audio](const PeerRequest& req) {
      return audio.handlePeerRequest(req);
    });

    // A real item so the routed command can actually mutate the timeline.
    Result addRes = engine_->call(QStringLiteral("text.add"), QJsonObject{
      {QStringLiteral("text"), QStringLiteral("Title")},
      {QStringLiteral("startFrame"), 0},
      {QStringLiteral("durationFrames"), 60}
    });
    QVERIFY(addRes.ok);
    const QString itemId = addRes.value.toObject().value(QStringLiteral("items")).toArray().at(0).toObject()
                             .value(QStringLiteral("id")).toString();
    QVERIFY(!itemId.isEmpty());

    // grade_clip now executes the real item.color.set command: the grade actually lands.
    PeerResponse resp = editor.sendPeerRequest(
      SpecialistKind::Colourist,
      QStringLiteral("grade_clip"),
      QJsonObject{{QStringLiteral("itemId"), itemId},
                  {QStringLiteral("primaries"), QJsonObject{{QStringLiteral("exposure"), 0.5}}}},
      {},
      QStringLiteral("warm look")
    );
    QCOMPARE(resp.status, QStringLiteral("done"));
    QCOMPARE(resp.note, QStringLiteral("executed item.color.set"));
    QCOMPARE(resp.outputs.value(QStringLiteral("ok")).toBool(), true);

    // ... and the timeline really changed (the review scenario: "grade applied" with an untouched timeline).
    Result itemRes = engine_->call(QStringLiteral("item.get"), QJsonObject{{QStringLiteral("itemId"), itemId}});
    QVERIFY(itemRes.ok);
    const QJsonObject color = itemRes.value.toObject().value(QStringLiteral("color")).toObject();
    QCOMPARE(color.value(QStringLiteral("primaries")).toObject().value(QStringLiteral("exposure")).toDouble(), 0.5);

    // A peer kind the recipient cannot serve is reported honestly as unsupported.
    PeerResponse unknown = editor.sendPeerRequest(
      SpecialistKind::Audio, QStringLiteral("make_sandwich"), {}, {}, QStringLiteral("impossible")
    );
    QCOMPARE(unknown.status, QStringLiteral("unsupported"));
    QVERIFY(unknown.note.contains(QStringLiteral("make_sandwich")));

    // duck_music maps to the real audio.duck command: both calls decline with the engine's
    // own error instead of a canned success. The legacy {volume} peer argument no longer
    // matches audio.duck's schema (it takes depthDb), so validation reports it honestly;
    // without any arguments the command itself reports that no music track can be guessed.
    PeerResponse duckLegacyVol = editor.sendPeerRequest(
      SpecialistKind::Audio, QStringLiteral("duck_music"),
      QJsonObject{{QStringLiteral("volume"), 0.3}}, {}, QStringLiteral("dialogue at 0:10")
    );
    QCOMPARE(duckLegacyVol.status, QStringLiteral("declined"));
    QVERIFY2(duckLegacyVol.note.contains(QStringLiteral("volume")), qPrintable(duckLegacyVol.note));

    PeerResponse duck = editor.sendPeerRequest(
      SpecialistKind::Audio, QStringLiteral("duck_music"), {}, {}, QStringLiteral("dialogue at 0:10")
    );
    QCOMPARE(duck.status, QStringLiteral("declined"));
    QVERIFY2(duck.note.contains(QStringLiteral("musicTrack")), qPrintable(duck.note));

    auto log = bus_->teamThread();
    QCOMPARE(static_cast<int>(log.size()), 4);
    QCOMPARE(log[0].from, SpecialistKind::Editor);
    QCOMPARE(log[0].to, SpecialistKind::Colourist);
    QCOMPARE(log[0].kind, QStringLiteral("grade_clip"));
    QCOMPARE(log[0].status, QStringLiteral("done"));
    QCOMPARE(log[1].status, QStringLiteral("unsupported"));
    QCOMPARE(log[2].status, QStringLiteral("declined"));
    QCOMPARE(log[3].status, QStringLiteral("declined"));
  }

  // ---- missing LLM client fails the turn instead of faking ok ----
  void noLlmClientFailsTurn() {
    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get()); // no LLM client attached

    QJsonObject turn = editor.processTurn(QStringLiteral("cut the clip"));
    QCOMPARE(turn.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(turn.value(QStringLiteral("error")).toString(), QStringLiteral("no_llm_client"));
    QCOMPARE(turn.value(QStringLiteral("content")).toString(), QString());
    // The user message is still recorded before failing, so history stays consistent.
    QCOMPARE(editor.history().size(), static_cast<size_t>(1));
  }

  // ---- tool loop feeds each result back to the model as a role:"tool" message ----
  void toolLoopFeedsToolResultToLlm() {
    auto mock = std::make_shared<MockLlmClient>();
    mock->enqueueResponse(QJsonObject{
      {QStringLiteral("content"), QString()},
      {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("call_1")},
        {QStringLiteral("function"), QJsonObject{
          {QStringLiteral("name"), QStringLiteral("timeline.get")},
          {QStringLiteral("arguments"), QStringLiteral("{}")}
        }}
      }}}
    });
    mock->setDefaultResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Timeline read.")}});

    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get(), mock);
    QJsonObject turn = editor.processTurn(QStringLiteral("summarize the timeline"));

    QCOMPARE(turn.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(turn.value(QStringLiteral("content")).toString(), QStringLiteral("Timeline read."));
    // Round 1 (the second model call) must contain the tool result message fed back.
    QCOMPARE(mock->calls().size(), static_cast<size_t>(2));
    bool sawToolMessage = false;
    for (const auto& m : mock->calls()[1].messages) {
      if (m.role != QStringLiteral("tool")) continue;
      sawToolMessage = true;
      QCOMPARE(m.toolCallId, QStringLiteral("call_1"));
      QVERIFY(m.content.contains(QStringLiteral("\"ok\"")));
    }
    QVERIFY(sawToolMessage);

    const QJsonArray rows = turn.value(QStringLiteral("tool_results")).toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("call_id")).toString(), QStringLiteral("call_1"));
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("result")).toObject().value(QStringLiteral("ok")).toBool(), true);
  }

  // ---- the tool loop has no round cap: it runs until the model stops calling tools ----
  void toolLoopRunsUntilModelStops() {
    constexpr int kRounds = 20; // well past the old cap of 8
    auto mock = std::make_shared<MockLlmClient>();
    for (int i = 0; i < kRounds; ++i) {
      mock->enqueueResponse(QJsonObject{
        {QStringLiteral("content"), QString()},
        {QStringLiteral("tool_calls"), QJsonArray{QJsonObject{
          {QStringLiteral("id"), QStringLiteral("call_%1").arg(i)},
          {QStringLiteral("function"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("timeline.get")},
            {QStringLiteral("arguments"), QStringLiteral("{}")}
          }}
        }}}
      });
    }
    mock->enqueueResponse(QJsonObject{{QStringLiteral("content"), QStringLiteral("Done.")}});

    Specialist editor(SpecialistKind::Editor, engine_.get(), bus_.get(), mock);
    QJsonObject turn = editor.processTurn(QStringLiteral("keeps calling tools, then answers"));

    QCOMPARE(turn.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QVERIFY(!turn.contains(QStringLiteral("error")));
    QCOMPARE(mock->calls().size(), static_cast<size_t>(kRounds + 1));
  }
};

QTEST_MAIN(TstAgentSpecialists)
#include "tst_agent_specialists.moc"
