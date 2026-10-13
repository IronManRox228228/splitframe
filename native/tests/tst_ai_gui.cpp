// The GUI's engine carries the AI commands: Editor::engine() is attached to the Editor's own project, so the agent edits the live
// document (window, undo history) with the agent as the actor. Mock model only.

#include "app/editor.h"
#include "app/player.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }
const char* kMarkerScript =
    "{\"content\":\"Dropped a marker.\",\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"marker.add\","
    "\"arguments\":\"{\\\"frame\\\":10,\\\"label\\\":\\\"gui\\\"}\"}}]}";
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
};

QTEST_MAIN(TstAiGui)
#include "tst_ai_gui.moc"
