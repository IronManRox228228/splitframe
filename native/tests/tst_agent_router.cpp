// Unit tests for the thin stateless router in M5b.

#include "agent/router.h"

#include <QTest>

using namespace sf::agent;

class TstAgentRouter : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {}

  // ---- intent detection ----
  void singleIntents() {
    Router r;
    {
      RouteResult res = r.route(QStringLiteral("cut the pauses in this clip"));
      QVERIFY(res.intents.contains(QStringLiteral("pauses")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
      QVERIFY(!res.needsPlan);
    }
    {
      RouteResult res = r.route(QStringLiteral("remove all um and uh fillers"));
      QVERIFY(res.intents.contains(QStringLiteral("fillers")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
    }
    {
      RouteResult res = r.route(QStringLiteral("add subtitles with yellow text"));
      QVERIFY(res.intents.contains(QStringLiteral("captions")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
    }
    {
      RouteResult res = r.route(QStringLiteral("export this project as 1080p mp4"));
      QVERIFY(res.intents.contains(QStringLiteral("export")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
    }
    {
      RouteResult res = r.route(QStringLiteral("undo that last edit"));
      QVERIFY(res.intents.contains(QStringLiteral("undo")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
    }
  }

  // ---- domain assignment: colourist ----
  void colouristRouting() {
    Router r;
    {
      RouteResult res = r.route(QStringLiteral("apply a teal and orange LUT to the shots"));
      QVERIFY(res.intents.contains(QStringLiteral("color")));
      QCOMPARE(res.primary, SpecialistKind::Colourist);
      QVERIFY(res.secondary.empty());
    }
    {
      RouteResult res = r.route(QStringLiteral("boost saturation and lift gamma on this interview"));
      QVERIFY(res.intents.contains(QStringLiteral("color")));
      QCOMPARE(res.primary, SpecialistKind::Colourist);
    }
  }

  // ---- domain assignment: audio ----
  void audioRouting() {
    Router r;
    {
      RouteResult res = r.route(QStringLiteral("duck the background music under the speech track"));
      QVERIFY(res.intents.contains(QStringLiteral("music")));
      QCOMPARE(res.primary, SpecialistKind::Audio);
    }
    {
      RouteResult res = r.route(QStringLiteral("detect the beat and bpm of the rhythm track"));
      QVERIFY(res.intents.contains(QStringLiteral("beats")));
      QCOMPARE(res.primary, SpecialistKind::Audio);
    }
    {
      RouteResult res = r.route(QStringLiteral("adjust volume and master mixer loudness to -14 LUFS"));
      QVERIFY(res.intents.contains(QStringLiteral("audio")));
      QCOMPARE(res.primary, SpecialistKind::Audio);
    }
  }

  // ---- cross-domain collaboration & planning ----
  void crossDomainNeedsPlan() {
    Router r;
    {
      RouteResult res = r.route(QStringLiteral("cut the pauses and add background music"));
      QVERIFY(res.intents.contains(QStringLiteral("pauses")));
      QVERIFY(res.intents.contains(QStringLiteral("music")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
      QVERIFY(!res.secondary.empty());
      QCOMPARE(res.secondary[0], SpecialistKind::Audio);
      QVERIFY(res.needsPlan);
    }
    {
      RouteResult res = r.route(QStringLiteral("assemble a 30s teaser with warm colour grade and music"));
      QVERIFY(res.intents.contains(QStringLiteral("assemble")));
      QVERIFY(res.intents.contains(QStringLiteral("color")));
      QVERIFY(res.intents.contains(QStringLiteral("music")));
      QCOMPARE(res.primary, SpecialistKind::Editor);
      QVERIFY(res.needsPlan);
    }
  }

  // ---- questions ----
  void questionsAreNonMutating() {
    Router r;
    {
      RouteResult res = r.route(QStringLiteral("What is the current timeline duration?"));
      QVERIFY(res.isQuestion);
      QCOMPARE(res.via, QStringLiteral("question"));
      QVERIFY(!res.needsPlan);
    }
    {
      RouteResult res = r.route(QStringLiteral("How do I export this project?"));
      QVERIFY(res.isQuestion);
    }
  }

  // ---- follow-up context ----
  void contextFollowUp() {
    Router r;
    RouteResult res = r.route(QStringLiteral("make them bigger and bolder"), QStringLiteral("Added captions at 0:15"));
    QVERIFY(res.intents.contains(QStringLiteral("captions")));
    QCOMPARE(res.primary, SpecialistKind::Editor);
  }
};

QTEST_MAIN(TstAgentRouter)
#include "tst_agent_router.moc"
