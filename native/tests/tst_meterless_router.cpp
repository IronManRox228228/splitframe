// Unit test per router rule (meterless-power-prompt.md §14-29): the class comes from the task, the
// disposition from the user pick with per-task pins beating both, the attachment rule, the
// meterless-accept rule, the 3d route rule and the downgrade rules.

#include "agent/meterless_router.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sf::meterless_router;

class TestMeterlessRouter : public QObject {
  Q_OBJECT

private slots:
  // Rule: every call site declares a task; the task picks the class (commentary → text, grade →
  // judge, screenshot → vision, still → image, voice → speech, theme → music, model → mesh,
  // repaint → texture), plus SplitFrame's call sites (planning/tool-calling → text, storyboard and
  // generation-prompt → text, peer review → judge, frame grab → vision).
  void taskPicksTheClass() {
    QCOMPARE(taskRoute(QStringLiteral("commentary")).className, QStringLiteral("text"));
    QCOMPARE(taskRoute(QStringLiteral("planning")).className, QStringLiteral("text"));
    QCOMPARE(taskRoute(QStringLiteral("tool_call")).className, QStringLiteral("text"));
    QCOMPARE(taskRoute(QStringLiteral("generation_prompt")).className, QStringLiteral("text"));
    QCOMPARE(taskRoute(QStringLiteral("storyboard")).className, QStringLiteral("text"));
    QCOMPARE(taskRoute(QStringLiteral("grade")).className, QStringLiteral("judge"));
    QCOMPARE(taskRoute(QStringLiteral("peer_review")).className, QStringLiteral("judge"));
    QCOMPARE(taskRoute(QStringLiteral("screenshot")).className, QStringLiteral("vision"));
    QCOMPARE(taskRoute(QStringLiteral("frame_grab")).className, QStringLiteral("vision"));
    QCOMPARE(taskRoute(QStringLiteral("still")).className, QStringLiteral("image"));
    QCOMPARE(taskRoute(QStringLiteral("voice")).className, QStringLiteral("speech"));
    QCOMPARE(taskRoute(QStringLiteral("theme")).className, QStringLiteral("music"));
    QCOMPARE(taskRoute(QStringLiteral("model_3d")).className, QStringLiteral("mesh"));
    QCOMPARE(taskRoute(QStringLiteral("repaint")).className, QStringLiteral("texture"));
    QCOMPARE(taskRoute(QStringLiteral("totally-unknown")).className, QString());
  }

  // Rule: the disposition comes from the user's single pick in Settings (SplitFrame default fast).
  void dispositionFromTheUserPick() {
    QCOMPARE(planRoute(QStringLiteral("commentary")).value(QStringLiteral("id")).toString(),
             QStringLiteral("meterless/text.fast"));
    RouterOptions deep;
    deep.disposition = QStringLiteral("deep");
    QJsonObject planned = planRoute(QStringLiteral("planning"), {}, deep);
    QCOMPARE(planned.value(QStringLiteral("id")).toString(), QStringLiteral("meterless/text.deep"));
    QCOMPARE(planned.value(QStringLiteral("disposition")).toString(), QStringLiteral("deep"));
  }

  // Rule: a per-task pin (meterless/image.local for stills only) beats both the task route and the
  // user pick, for that task only.
  void perTaskPinBeatsBoth() {
    RouterOptions pinned;
    pinned.disposition = QStringLiteral("deep");
    pinned.pins = QJsonObject{{QStringLiteral("still"), QStringLiteral("meterless/image.local")}};
    QJsonObject planned = planRoute(QStringLiteral("still"), {}, pinned);
    QCOMPARE(planned.value(QStringLiteral("id")).toString(), QStringLiteral("meterless/image.local"));
    QCOMPARE(planned.value(QStringLiteral("source")).toString(), QStringLiteral("pin"));
    // Another task keeps the user's pick.
    QCOMPARE(planRoute(QStringLiteral("commentary"), {}, pinned).value(QStringLiteral("id")).toString(),
             QStringLiteral("meterless/text.deep"));
  }

  // Rule: a task never inherits its class from whichever model is selected — pins re-point the id
  // for that task, the model choice (disposition) never rewrites the class.
  void classNeverInheritedFromTheModel() {
    RouterOptions opts;
    opts.disposition = QStringLiteral("deep");
    QCOMPARE(planRoute(QStringLiteral("still"), {}, opts).value(QStringLiteral("class")).toString(), QStringLiteral("image"));
    QCOMPARE(planRoute(QStringLiteral("commentary"), {}, opts).value(QStringLiteral("class")).toString(), QStringLiteral("text"));
  }

  // Rule: attachments reclassify only text-output tasks (audio/video → transcribe, image → vision,
  // PDF/document → perception). "Make a still from this screenshot" stays image.
  void attachmentRule() {
    QJsonObject planned = planRoute(QStringLiteral("commentary"), {QStringLiteral("image")});
    QCOMPARE(planned.value(QStringLiteral("class")).toString(), QStringLiteral("vision"));
    QCOMPARE(planned.value(QStringLiteral("source")).toString(), QStringLiteral("attachment"));

    QCOMPARE(planRoute(QStringLiteral("planning"), {QStringLiteral("audio")}).value(QStringLiteral("class")).toString(),
             QStringLiteral("transcribe"));
    QCOMPARE(planRoute(QStringLiteral("planning"), {QStringLiteral("video")}).value(QStringLiteral("class")).toString(),
             QStringLiteral("transcribe"));
    QCOMPARE(planRoute(QStringLiteral("generation_prompt"), {QStringLiteral("pdf")}).value(QStringLiteral("class")).toString(),
             QStringLiteral("perception"));
    QCOMPARE(planRoute(QStringLiteral("generation_prompt"), {QStringLiteral("document")}).value(QStringLiteral("class")).toString(),
             QStringLiteral("perception"));

    // A media task keeps its class even with the same attachment.
    QCOMPARE(planRoute(QStringLiteral("still"), {QStringLiteral("image")}).value(QStringLiteral("class")).toString(),
             QStringLiteral("image"));
    QCOMPARE(planRoute(QStringLiteral("still"), {QStringLiteral("image")}).value(QStringLiteral("source")).toString(),
             QStringLiteral("task"));
  }

  // Rule: meterless-accept per request, only for the media the route produces.
  void acceptHeaderRule() {
    QCOMPARE(planRoute(QStringLiteral("screenshot")).value(QStringLiteral("accept")).toString(), QString());
    QCOMPARE(planRoute(QStringLiteral("still")).value(QStringLiteral("accept")).toString(), QStringLiteral("image"));
    QCOMPARE(planRoute(QStringLiteral("voice")).value(QStringLiteral("accept")).toString(), QStringLiteral("audio"));
    QCOMPARE(planRoute(QStringLiteral("theme")).value(QStringLiteral("accept")).toString(), QStringLiteral("audio"));
    QCOMPARE(planRoute(QStringLiteral("preview_render")).value(QStringLiteral("accept")).toString(), QStringLiteral("video"));
    QCOMPARE(planRoute(QStringLiteral("repaint")).value(QStringLiteral("accept")).toString(), QStringLiteral("mesh"));
  }

  // Rule: mesh and texture are served on POST /v1/meterless/3d, never on chat; planRoute marks them
  // route "3d".
  void threeDRouteRule() {
    QCOMPARE(planRoute(QStringLiteral("model_3d")).value(QStringLiteral("route")).toString(), QStringLiteral("3d"));
    QCOMPARE(planRoute(QStringLiteral("repaint")).value(QStringLiteral("route")).toString(), QStringLiteral("3d"));
    QCOMPARE(planRoute(QStringLiteral("screenshot")).value(QStringLiteral("route")).toString(), QStringLiteral("chat"));
    QCOMPARE(planRoute(QStringLiteral("commentary")).value(QStringLiteral("route")).toString(), QStringLiteral("chat"));
  }

  // Rule: fusion on a single-core class (video, music, speech, embed, mesh, texture) and cluster on
  // vision, perception, transcribe or embed are downgraded to auto before sending, and the downgrade
  // is recorded.
  void downgradeRules() {
    QJsonObject planned = planRoute(QStringLiteral("theme"), {}, RouterOptions{QStringLiteral("fusion")});
    QCOMPARE(planned.value(QStringLiteral("disposition")).toString(), QStringLiteral("auto"));
    QCOMPARE(planned.value(QStringLiteral("downgrade")).toString(), QStringLiteral("fusion"));

    QCOMPARE(planRoute(QStringLiteral("commentary"), {}, RouterOptions{QStringLiteral("fusion")})
               .value(QStringLiteral("disposition")).toString(), QStringLiteral("fusion")); // text allows fusion
    QCOMPARE(planRoute(QStringLiteral("screenshot"), {}, RouterOptions{QStringLiteral("cluster")})
               .value(QStringLiteral("downgrade")).toString(), QStringLiteral("cluster"));
    QCOMPARE(planRoute(QStringLiteral("screenshot"), {}, RouterOptions{QStringLiteral("cluster")})
               .value(QStringLiteral("disposition")).toString(), QStringLiteral("auto"));
    QVERIFY(!planRoute(QStringLiteral("commentary"), {}, RouterOptions{QStringLiteral("cluster")})
               .contains(QStringLiteral("downgrade")));
  }

  // Rule: which classes allow fusion is read from GET /v1/meterless/modes when it answers; the
  // hardcoded single-core list is only the fallback.
  void runtimeFusionListOverridesTheFallback() {
    RouterOptions runtime;
    runtime.disposition = QStringLiteral("fusion");
    runtime.runtimeModes = QJsonObject{{QStringLiteral("fusion"), QJsonArray{QStringLiteral("video"), QStringLiteral("text")}}};
    QCOMPARE(planRoute(QStringLiteral("theme"), {}, runtime).value(QStringLiteral("disposition")).toString(), QStringLiteral("auto"));
    QCOMPARE(planRoute(QStringLiteral("theme"), {}, runtime).value(QStringLiteral("downgrade")).toString(), QStringLiteral("fusion"));
    // video IS in the runtime's allowed list, so fusion survives there; screenshot (vision) does not.
    QCOMPARE(planRoute(QStringLiteral("preview_render"), {}, runtime).value(QStringLiteral("disposition")).toString(), QStringLiteral("fusion"));
    QVERIFY(!planRoute(QStringLiteral("preview_render"), {}, runtime).contains(QStringLiteral("downgrade")));
    QCOMPARE(planRoute(QStringLiteral("screenshot"), {}, runtime).value(QStringLiteral("downgrade")).toString(), QStringLiteral("fusion"));
    // When the runtime marks cluster unavailable it is refused, never swapped for auto.
    runtime.disposition = QStringLiteral("cluster");
    runtime.runtimeModes = QJsonObject{{QStringLiteral("cluster"), false}};
    QCOMPARE(planRoute(QStringLiteral("screenshot"), {}, runtime).value(QStringLiteral("error")).toString(),
             QStringLiteral("cluster_unavailable"));
  }

  // The Settings routing table: one read-only row per call site task.
  void routingTableRows() {
    const QJsonArray rows = routingTable();
    QVERIFY(rows.size() > 10);
    bool foundCommentary = false, foundRepaint = false;
    for (const QJsonValue& v : rows) {
      const QJsonObject row = v.toObject();
      if (row.value(QStringLiteral("task")).toString() == QStringLiteral("commentary")) {
        foundCommentary = true;
        QCOMPARE(row.value(QStringLiteral("class")).toString(), QStringLiteral("text"));
        QCOMPARE(row.value(QStringLiteral("route")).toString(), QStringLiteral("chat"));
      }
      if (row.value(QStringLiteral("task")).toString() == QStringLiteral("repaint")) {
        foundRepaint = true;
        QCOMPARE(row.value(QStringLiteral("class")).toString(), QStringLiteral("texture"));
        QCOMPARE(row.value(QStringLiteral("route")).toString(), QStringLiteral("3d"));
        QCOMPARE(row.value(QStringLiteral("meterless-accept")).toString(), QStringLiteral("mesh"));
      }
    }
    QVERIFY(foundCommentary);
    QVERIFY(foundRepaint);
  }
};

QTEST_MAIN(TestMeterlessRouter)
#include "tst_meterless_router.moc"
