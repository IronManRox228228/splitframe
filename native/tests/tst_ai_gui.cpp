// The GUI's engine carries the AI commands: Editor::engine() is attached to the Editor's own project, so the agent edits the live
// document (window, undo history) with the agent as the actor. Mock model only.

#include "app/editor.h"
#include "app/player.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }
// The specialist loops tool rounds until the model stops calling tools: the default reply calls marker.add, and once the
// tool result (which carries the "gui" label) is in the conversation the scripted reply ends the turn.
const char* kMarkerScript =
    "{\"default\":{\"content\":\"Dropping a marker.\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"marker.add\","
    "\"arguments\":\"{\\\"frame\\\":10,\\\"label\\\":\\\"gui\\\"}\"}}]},"
    "\"scripted\":[{\"trigger\":\"\\\"gui\\\"\",\"response\":{\"content\":\"Dropped a marker.\"}}]}";
// The Director's own mutating tool is the storyboard (it delegates the rest): one scene per call, undoable.
const char* kSceneScript =
    "{\"default\":{\"content\":\"Adding a scene.\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"storyboard.scene.add\","
    "\"arguments\":\"{\\\"title\\\":\\\"gui\\\"}\"}}]},"
    "\"scripted\":[{\"trigger\":\"\\\"gui\\\"\",\"response\":{\"content\":\"Added a scene.\"}}]}";
} // namespace

class TstAiGui : public QObject {
  Q_OBJECT
private slots:
  void theEditorEngineExposesAndRunsTheAgentCommands() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(Q("mock.json"));
    QFile f(script);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(kMarkerScript);
    f.close();

    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("mock");
    options.ai.llm.mockScript = script;
    app::Editor editor(player, options);

    engine::Engine* e = editor.engine();
    QVERIFY(e);
    QCOMPARE(editor.engine(), e);
    for (const char* n : {"agent.turn", "agent.route", "copilot.lint.run", "branch.list", "index.search", "timeline.get"}) QVERIFY2(e->find(Q(n)), n);

    QVERIFY(!editor.canUndo());
    const engine::Result r = e->call(Q("agent.turn"), {{Q("message"), Q("add a marker at frame 10")}, {Q("mode"), Q("auto")}});
    QVERIFY2(r.ok, qPrintable(r.error.message));
    QCOMPARE(static_cast<int>(editor.project()->doc().markers.size()), 1); // the GUI's own project
    QVERIFY(editor.canUndo());                                           // in its undo history
    const QJsonArray entries = e->call(Q("history.list")).value.toObject().value(Q("entries")).toArray();
    QCOMPARE(entries.last().toObject().value(Q("actor")).toString(), Q("agent:editor"));
    editor.undo();
    QCOMPARE(static_cast<int>(editor.project()->doc().markers.size()), 0);
    QVERIFY(e->call(Q("branch.list")).ok);
    QVERIFY(e->call(Q("copilot.lint.run")).ok);
  }

  // The Assistant's bridge: agentChat returns at once, the crew's agent.event stream reaches the GUI as agentEvent (through the
  // engine's own event bus), and agentTurnFinished closes the run.
  void agentChatReturnsAtOnceAndStreamsRealEvents() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(Q("mock.json"));
    QFile f(script);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"content\":\"Nothing to change.\"}");
    f.close();

    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("mock");
    options.ai.llm.mockScript = script;
    app::Editor editor(player, options);

    QSignalSpy events(&editor, &app::Editor::agentEvent);
    QSignalSpy finished(&editor, &app::Editor::agentTurnFinished);
    QVERIFY(!editor.agentBusy());
    editor.agentChat(Q("tidy the cut"));
    QVERIFY(editor.agentBusy());      // set synchronously ...
    QCOMPARE(finished.count(), 0);     // ... and nothing has run yet: the call did not block
    editor.agentChat(Q("ignored while busy"));
    QVERIFY(finished.wait(10000));
    QVERIFY(!editor.agentBusy());
    QCOMPARE(finished.count(), 1);

    QStringList kinds;
    for (const QList<QVariant>& args : events) {
      const QVariantMap e = args.first().toMap();
      kinds << e.value(Q("kind")).toString();
      QCOMPARE(e.value(Q("agent")).toString(), Q("director"));
    }
    QVERIFY2(kinds.contains(Q("think")), qPrintable(kinds.join(Q(","))));
    QVERIFY2(kinds.contains(Q("message")), qPrintable(kinds.join(Q(","))));
    QVERIFY(finished.first().first().toMap().value(Q("response")).toString().contains(Q("Nothing to change")));

    // the pill's mode is the engine's mode
    editor.setAgentMode(Q("ask"));
    editor.setAgentMode(Q("bogus"));
    QCOMPARE(editor.agentMode(), Q("ask"));
    QCOMPARE(editor.storyboard().size(), 0);
  }

  // Copilot lock at the GUI: from the moment the Assistant is asked until its run ends, direct edits (Project, Editor undo) are
  // refused with the busy message, then work again; and the run is one undo step.
  void theHumanCannotEditWhileTheAssistantRuns() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(Q("mock.json"));
    QFile f(script);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(kSceneScript);
    f.close();

    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("mock");
    options.ai.llm.mockScript = script;
    app::Editor editor(player, options);
    QVERIFY(editor.project()->apply(ProjectRename{Q("Human 1")}, Q("Rename"))); // an earlier human step
    QSignalSpy busyChanged(&editor, &app::Editor::agentBusyChanged);
    QSignalSpy toasts(&editor, &app::Editor::toastRequested);
    QSignalSpy finished(&editor, &app::Editor::agentTurnFinished);

    editor.agentChat(Q("add a marker at frame 10"));
    QVERIFY(editor.agentBusy());
    QCOMPARE(editor.agentBusyName(), Q("Director"));
    QVERIFY(busyChanged.count() >= 1);
    // refused before the run has even started (no gap), and again once its group is open
    QVERIFY(!editor.project()->apply(ProjectRename{Q("Human 2")}, Q("Rename")));
    QVERIFY(editor.project()->lastError().contains(Q("is editing")));
    QCOMPARE(editor.project()->name(), Q("Human 1"));
    const int undoPos = static_cast<int>(editor.project()->history().position());
    editor.undo();
    editor.redo();
    QCOMPARE(static_cast<int>(editor.project()->history().position()), undoPos);
    bool sawNotice = false;
    for (const QList<QVariant>& t : toasts) sawNotice = sawNotice || t.at(0).toString().contains(Q("Wait for it to finish"));
    QVERIFY2(sawNotice, "a refused edit shows a notice, not silence");
    QVERIFY(!editor.controller()->beginMove(Q("nope"), 0, false)); // drags do not start

    QVERIFY(finished.wait(10000));
    QVERIFY(!editor.agentBusy());
    QVERIFY(editor.agentBusyName().isEmpty());
    QCOMPARE(static_cast<int>(editor.project()->doc().storyboard.size()), 1);
    QVERIFY(editor.project()->apply(ProjectRename{Q("Human 2")}, Q("Rename"))); // unlocked
    editor.undo(); // the rename
    editor.undo(); // the whole agent run in one step
    QCOMPARE(static_cast<int>(editor.project()->doc().storyboard.size()), 0);
    QCOMPARE(editor.project()->name(), Q("Human 1")); // the earlier human step is untouched
  }

  // Stop: the run ends after the tool call it is on, keeps what it did as one undo step, and the lock releases.
  void stopEndsTheRunKeepsOneUndoStepAndUnlocks() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(Q("mock.json"));
    QFile f(script);
    QVERIFY(f.open(QIODevice::WriteOnly));
    // never stops by itself: every reply calls marker.add again (the loop would run its 8 rounds)
    f.write("{\"content\":\"More scenes.\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"storyboard.scene.add\","
            "\"arguments\":\"{\\\"title\\\":\\\"gui\\\"}\"}}]}");
    f.close();

    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("mock");
    options.ai.llm.mockScript = script;
    app::Editor editor(player, options);
    QSignalSpy finished(&editor, &app::Editor::agentTurnFinished);
    bool stopped = false;
    connect(&editor, &app::Editor::agentEvent, &editor, [&](const QVariantMap& ev) {
      if (!stopped && ev.value(Q("kind")).toString() == Q("result")) {
        stopped = true;
        editor.agentStop();
      }
    });
    editor.agentChat(Q("add markers"));
    QVERIFY(finished.wait(10000));
    QVERIFY(!editor.agentBusy());
    QCOMPARE(static_cast<int>(editor.project()->doc().storyboard.size()), 1); // stopped after the first call: kept
    QVERIFY(editor.project()->apply(ProjectRename{Q("After stop")}, Q("Rename")));
    editor.undo();
    editor.undo();
    QCOMPARE(static_cast<int>(editor.project()->doc().storyboard.size()), 0); // the run was one step

    // Stop pressed before the run starts: nothing runs, the lock still releases
    editor.agentChat(Q("add markers"));
    editor.agentStop();
    QVERIFY(finished.wait(10000));
    QVERIFY(!editor.agentBusy());
    QCOMPARE(static_cast<int>(editor.project()->doc().storyboard.size()), 0);
    QVERIFY(editor.project()->apply(ProjectRename{Q("Again")}, Q("Rename")));
  }

  // A group an outside agent (API, MCP) left open: the window shows it as busy and Stop takes the project back.
  void stopClosesAGroupAnOutsideAgentLeftOpen() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("mock");
    app::Editor editor(player, options);
    engine::Engine* e = editor.engine();
    QVERIFY(e->call(Q("session.group.begin"), {{Q("label"), Q("Outside run")}, {Q("actor"), Q("agent:claude")}}).ok);
    QVERIFY(e->call(Q("marker.add"), {{Q("frame"), 3}, {Q("label"), Q("outside")}, {Q("origin"), Q("agent:claude")}}).ok);
    QVERIFY(editor.agentBusy());
    QCOMPARE(editor.agentBusyName(), Q("Claude"));
    QVERIFY(!editor.project()->apply(ProjectRename{Q("nope")}, Q("Rename")));
    editor.agentStop();
    QVERIFY(!editor.agentBusy());
    QVERIFY(editor.project()->apply(ProjectRename{Q("mine")}, Q("Rename")));
    editor.undo();
    editor.undo();
    QCOMPARE(static_cast<int>(editor.project()->doc().markers.size()), 0);
  }

  // A model that is not there: the failure arrives as a code the Assistant turns into plain language (never a hang).
  void anUnreachableModelEndsTheRunWithAProviderCode() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    app::Player player;
    app::Editor::Options options;
    options.settingsFile = dir.filePath(Q("settings.ini"));
    options.recoveryDir = dir.filePath(Q("recovery"));
    options.autosaveMs = 0;
    options.ai.llm.endpoint = Q("http://127.0.0.1:9/v1/chat/completions");
    options.ai.llm.timeoutMs = 2000;
    app::Editor editor(player, options);
    QSignalSpy finished(&editor, &app::Editor::agentTurnFinished);
    editor.agentChat(Q("hello"));
    QVERIFY(finished.wait(15000));
    const QVariantMap r = finished.first().first().toMap();
    QVERIFY2(r.value(Q("error")).toString() == Q("provider_unreachable"), qPrintable(r.value(Q("error")).toString()));
  }
};

QTEST_MAIN(TstAiGui)
#include "tst_ai_gui.moc"
