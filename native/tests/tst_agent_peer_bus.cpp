// Unit tests for the PeerBus inter-agent communication channel in M5b.

#include "agent/peer_bus.h"

#include <QSignalSpy>
#include <QTest>

using namespace sf::agent;

class TstAgentPeerBus : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {}

  // ---- basic dispatch ----
  void basicDispatch() {
    PeerBus bus;
    bus.registerHandler(SpecialistKind::Audio, QStringLiteral("duck_music"), [](const PeerRequest& req) {
      PeerResponse res;
      res.status = QStringLiteral("done");
      res.note = QStringLiteral("music ducked to %1").arg(req.args.value(QStringLiteral("volume")).toDouble());
      res.outputs = QJsonObject{{QStringLiteral("ducked"), true}};
      return res;
    });

    PeerRequest req{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Audio,
      .kind = QStringLiteral("duck_music"),
      .args = QJsonObject{{QStringLiteral("volume"), 0.2}},
      .reason = QStringLiteral("duck under dialogue")
    };

    PeerResponse resp = bus.send(req);
    QCOMPARE(resp.status, QStringLiteral("done"));
    QCOMPARE(resp.outputs.value(QStringLiteral("ducked")).toBool(), true);
    QVERIFY(resp.note.contains(QStringLiteral("ducked to 0.2")));

    // Verify team thread audit log
    auto log = bus.teamThread();
    QCOMPARE(static_cast<int>(log.size()), 1);
    QCOMPARE(log[0].from, SpecialistKind::Editor);
    QCOMPARE(log[0].to, SpecialistKind::Audio);
    QCOMPARE(log[0].kind, QStringLiteral("duck_music"));
    QCOMPARE(log[0].status, QStringLiteral("done"));
  }

  // ---- self request rejected ----
  void selfRequestRejected() {
    PeerBus bus;
    PeerRequest req{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Editor,
      .kind = QStringLiteral("cut"),
      .reason = QStringLiteral("test")
    };

    PeerResponse resp = bus.send(req);
    QCOMPARE(resp.status, QStringLiteral("declined"));
    QVERIFY(resp.note.contains(QStringLiteral("self-request")));
  }

  // ---- chain depth limit ----
  void chainDepthLimit() {
    PeerBus bus;
    PeerRequest req{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Audio,
      .kind = QStringLiteral("deep_call"),
      .reason = QStringLiteral("test"),
      .depth = 4 // Exceeds kMaxChainDepth = 3
    };

    PeerResponse resp = bus.send(req);
    QCOMPARE(resp.status, QStringLiteral("declined"));
    QVERIFY(resp.note.contains(QStringLiteral("max chain depth")));
  }

  // ---- cycle detection ----
  void cycleDetection() {
    PeerBus bus;

    // Editor -> Audio -> Editor (cycle!)
    bus.registerHandler(SpecialistKind::Audio, QStringLiteral("ask_editor"), [&bus](const PeerRequest& req) {
      PeerRequest cycleReq{
        .from = SpecialistKind::Audio,
        .to = SpecialistKind::Editor,
        .kind = QStringLiteral("ask_back"),
        .reason = QStringLiteral("cycle attempt"),
        .depth = req.depth
      };
      return bus.send(cycleReq);
    });

    bus.registerHandler(SpecialistKind::Editor, QStringLiteral("ask_back"), [](const PeerRequest&) {
      return PeerResponse{.status = QStringLiteral("done")};
    });

    // Start with Editor in active context calling Audio
    PeerRequest rootReq{
      .from = SpecialistKind::Colourist,
      .to = SpecialistKind::Audio,
      .kind = QStringLiteral("ask_editor"),
      .reason = QStringLiteral("start chain")
    };

    PeerResponse resp = bus.send(rootReq);
    // Audio calls Editor, which succeeds because Editor wasn't in stack yet
    QCOMPARE(resp.status, QStringLiteral("done"));

    // Now test real cycle: Editor calls Audio, Audio calls Editor
    bus.registerHandler(SpecialistKind::Editor, QStringLiteral("start_cycle"), [&bus](const PeerRequest& req) {
      PeerRequest toAudio{
        .from = SpecialistKind::Editor,
        .to = SpecialistKind::Audio,
        .kind = QStringLiteral("cycle_to_audio"),
        .reason = QStringLiteral("call audio"),
        .depth = req.depth
      };
      return bus.send(toAudio);
    });

    bus.registerHandler(SpecialistKind::Audio, QStringLiteral("cycle_to_audio"), [&bus](const PeerRequest& req) {
      // Audio tries to call Editor back within same call stack
      PeerRequest backToEditor{
        .from = SpecialistKind::Audio,
        .to = SpecialistKind::Editor,
        .kind = QStringLiteral("back_to_editor"),
        .reason = QStringLiteral("call editor back"),
        .depth = req.depth
      };
      return bus.send(backToEditor);
    });

    PeerRequest testReq{
      .from = SpecialistKind::Colourist,
      .to = SpecialistKind::Editor,
      .kind = QStringLiteral("start_cycle"),
      .reason = QStringLiteral("kickoff")
    };

    PeerResponse cycleResp = bus.send(testReq);
    QCOMPARE(cycleResp.status, QStringLiteral("declined"));
    QVERIFY(cycleResp.note.contains(QStringLiteral("cycle detected")));
  }

  // ---- audit log json and clear ----
  void auditLogOperations() {
    PeerBus bus;
    bus.registerHandler(SpecialistKind::Colourist, QStringLiteral("grade"), [](const PeerRequest&) {
      return PeerResponse{.status = QStringLiteral("done"), .note = QStringLiteral("graded")};
    });

    PeerRequest req{
      .from = SpecialistKind::Editor,
      .to = SpecialistKind::Colourist,
      .kind = QStringLiteral("grade"),
      .reason = QStringLiteral("warm look")
    };
    bus.send(req);

    QJsonArray arr = bus.teamThreadJson();
    QCOMPARE(arr.size(), 1);
    QCOMPARE(arr[0].toObject().value(QStringLiteral("from")).toString(), QStringLiteral("editor"));
    QCOMPARE(arr[0].toObject().value(QStringLiteral("to")).toString(), QStringLiteral("colourist"));

    bus.clearLog();
    QCOMPARE(bus.teamThread().size(), static_cast<size_t>(0));
    QCOMPARE(bus.teamThreadJson().size(), 0);
  }
};

QTEST_MAIN(TstAgentPeerBus)
#include "tst_agent_peer_bus.moc"
