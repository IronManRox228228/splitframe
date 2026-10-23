// AI-WIRE: the agent, copilot, branch and index commands are reachable from every entry point (splitframe-cli call / commands /
// agent, an Engine attached to a live project the way the GUI does it) with the mock model only: no network, no llama-server.

#include "ai/ai_services.h"
#include "editor/media_pool.h"
#include "editor/project.h"
#include "engine/engine.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>

using namespace sf;
using namespace sf::engine;

namespace {
QString Q(const char* s) { return QString::fromLatin1(s); }

// A model that calls marker.add once, then answers in prose once the tool result comes back:
// the bounded agent loop feeds each result back as a role:"tool" message, so the second call
// must not ask for another marker. The stop trigger prefixes the tool-result JSON `{"ok":true...`,
// which only appears after marker.add ran (it sorts before "add a marker" in the trigger map).
const char* kMarkerScript =
    "{\"default\":{\"content\":\"All done.\"},\"scripted\":["
    "{\"trigger\":\"\\\"ok\\\":true\",\"response\":{\"content\":\"Dropped a marker at frame 15.\"}},"
    "{\"trigger\":\"add a marker\",\"response\":{\"content\":\"\","
    "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"marker.add\","
    "\"arguments\":\"{\\\"frame\\\":15,\\\"label\\\":\\\"wire\\\"}\"}}]}}"
    "]}";

const QString kDeadEndpoint = QStringLiteral("http://127.0.0.1:1/v1/chat/completions"); // nothing listens on port 1: refused at once
} // namespace

class TstAiWire : public QObject {
  Q_OBJECT

  QTemporaryDir dir_;
  QString project_, script_;

  struct Run {
    int code = -1;
    QString out, err;
    qint64 ms = 0;
    QJsonObject json() const { return QJsonDocument::fromJson(out.toUtf8()).object(); }
  };
  Run run(const QStringList& args, int timeoutMs = 60000) {
    QProcess p;
    QElapsedTimer t;
    t.start();
    p.start(QStringLiteral(SF_CLI_EXE), args);
    Run r;
    if (!p.waitForStarted(30000)) {
      r.err = p.errorString();
      return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
      p.kill();
      p.waitForFinished();
      r.err = Q("timed out");
      return r;
    }
    r.ms = t.elapsed();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
  }
  QStringList mock() const { return {Q("--llm-endpoint"), Q("mock"), Q("--llm-mock-script"), script_}; }
  static QStringList withArgs(QStringList a, const QStringList& more) {
    a << more;
    return a;
  }

  static ai::AiOptions mockOptions(const QString& script) {
    ai::AiOptions o;
    o.llm.endpoint = Q("mock");
    o.llm.mockScript = script;
    return o;
  }
  static int markersOf(Engine& e) { return e.call(Q("project.info")).value.toObject().value(Q("markers")).toInt(); }
  static QJsonArray history(Engine& e) { return e.call(Q("history.list")).value.toObject().value(Q("entries")).toArray(); }

private slots:
  void initTestCase() {
    QVERIFY(dir_.isValid());
    script_ = dir_.filePath(Q("mock.json"));
    QFile f(script_);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(kMarkerScript);
    f.close();
    // a small saved project, built through the engine itself
    Engine fx;
    QVERIFY(fx.call(Q("project.new"), {{Q("name"), Q("WireFixture")}, {Q("width"), 320}, {Q("height"), 180}, {Q("fps"), 25}}).ok);
    QVERIFY(fx.call(Q("text.add"), {{Q("text"), Q("Hello")}, {Q("startFrame"), 0}, {Q("durationFrames"), 50}}).ok);
    project_ = dir_.filePath(Q("p.json"));
    QVERIFY(fx.call(Q("project.saveAs"), {{Q("path"), project_}}).ok);
  }

  void commandsListIncludesTheAiCommands() {
    const Run r = run({Q("--json"), Q("commands"), Q("--llm-endpoint"), Q("mock")});
    QCOMPARE(r.code, 0);
    QSet<QString> names;
    QSet<QString> domains;
    for (const QJsonValue& v : r.json().value(Q("result")).toObject().value(Q("commands")).toArray()) {
      names.insert(v.toObject().value(Q("name")).toString());
      domains.insert(v.toObject().value(Q("domain")).toString());
    }
    for (const char* n : {"agent.turn", "agent.route", "agent.mode.get", "agent.mode.set", "agent.team.thread", "agent.specialists", "copilot.lint.run", "copilot.lint.fix", "copilot.suggest.get",
                          "copilot.review.run", "branch.create", "branch.switch", "branch.list", "branch.diff", "branch.merge", "branch.delete", "index.analyze", "index.search", "index.scenes",
                          "index.speech", "index.ocr", "index.status", "timeline.get"})
      QVERIFY2(names.contains(Q(n)), n);
    QVERIFY(domains.contains(Q("copilot")) && domains.contains(Q("branch")));
    // the same registry as an engine with the AI services built in-process
    Engine local;
    ai::AiServices services(local, mockOptions({}));
    QCOMPARE(names.size(), static_cast<int>(local.commands().size()));
    QVERIFY(names.size() > 55);
    // the domain filter reaches the new domains
    const Run c = run({Q("--json"), Q("commands"), Q("--domain"), Q("copilot")});
    QCOMPARE(c.code, 0);
    QVERIFY(c.json().value(Q("result")).toObject().value(Q("commands")).toArray().size() >= 3);
    QVERIFY(run({Q("--json"), Q("commands"), Q("--domain"), Q("branch")}).json().value(Q("result")).toObject().value(Q("commands")).toArray().size() >= 5);
  }

  // Part A items 1 and 3 (vendor-neutral surface): the provider is queryable. The local
  // OpenAI-compatible provider reports provider/base URL/model with a live reachability boolean;
  // the Meterless routing/lane surface is gone.
  void providerSurfaceCommands() {
    Engine e;
    ai::AiServices services(e, mockOptions(script_));
    bool hasStatus = false, hasRoute = false;
    for (const auto& c : e.commands()) {
      hasStatus |= c->name == Q("agent.provider.status");
      hasRoute |= c->name == Q("agent.provider.route");
    }
    QVERIFY(hasStatus);
    QVERIFY2(!hasRoute, "the Meterless routing command is gone");
    const Result status = e.call(Q("agent.provider.status"));
    QVERIFY(status.ok);
    const QJsonObject st = status.value.toObject();
    QCOMPARE(st.value(Q("provider")).toString(), Q("local"));
    QCOMPARE(st.value(Q("endpoint")).toString(), Q("mock"));
    QVERIFY(st.value(Q("reachable")).toBool());
    QVERIFY(st.value(Q("model")).toString().isEmpty() == false);
  }

  void agentCommandChangesTheProjectAndSaves() {
    const QString out = dir_.filePath(Q("agent_out.json"));
    Run r = run(withArgs({Q("--json"), Q("agent"), Q("add a marker at frame 15"), Q("--project"), project_, Q("--out"), out}, mock()));
    QCOMPARE(r.code, 0);
    QVERIFY2(r.json().value(Q("ok")).toBool(), qPrintable(r.out));
    const QJsonObject res = r.json().value(Q("result")).toObject();
    QCOMPARE(res.value(Q("mode")).toString(), Q("auto"));
    QCOMPARE(res.value(Q("route")).toObject().value(Q("primary")).toString(), Q("editor"));
    QVERIFY(res.value(Q("response")).toString().contains(Q("marker")));
    const QJsonArray calls = res.value(Q("toolResults")).toArray();
    QCOMPARE(calls.size(), 1);
    QCOMPARE(calls.at(0).toObject().value(Q("name")).toString(), Q("marker.add"));
    QVERIFY(calls.at(0).toObject().value(Q("result")).toObject().value(Q("ok")).toBool());
    const QJsonArray changes = res.value(Q("changes")).toArray();
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.at(0).toObject().value(Q("actor")).toString(), Q("agent:editor"));
    QVERIFY(res.value(Q("saved")).toBool());
    // the saved copy has the marker; the original file is untouched
    QCOMPARE(run({Q("--json"), Q("info"), out}).json().value(Q("result")).toObject().value(Q("markers")).toInt(), 1);
    QCOMPARE(run({Q("--json"), Q("info"), project_}).json().value(Q("result")).toObject().value(Q("markers")).toInt(), 0);
  }

  void agentCommandPrintsMessagesToolCallsAndChanges() {
    const Run r = run(withArgs({Q("agent"), Q("add a marker at frame 15"), Q("--project"), project_}, mock()));
    QCOMPARE(r.code, 0);
    QVERIFY2(r.out.contains(Q("routed to: editor")), qPrintable(r.out));
    QVERIFY(r.out.contains(Q("agent: Dropped a marker")));
    QVERIFY(r.out.contains(Q("marker.add")));
    QVERIFY(r.out.contains(Q("[agent:editor]")));
    QVERIFY(r.out.contains(Q("not saved")));
  }

  void planModeChangesNothing() {
    const Run r = run(withArgs({Q("--json"), Q("agent"), Q("add a marker at frame 15"), Q("--project"), project_, Q("--mode"), Q("plan")}, mock()));
    QCOMPARE(r.code, 0);
    QCOMPARE(r.json().value(Q("result")).toObject().value(Q("changes")).toArray().size(), 0);
    QCOMPARE(run({Q("agent"), Q("x"), Q("--project"), project_, Q("--mode"), Q("bogus")}).code, 2);
    QCOMPARE(run({Q("agent")}).code, 2);
  }

  void unreachableEndpointIsACleanUnavailableError() {
    Run r = run({Q("--json"), Q("agent"), Q("add a marker"), Q("--project"), project_, Q("--llm-endpoint"), kDeadEndpoint});
    QCOMPARE(r.code, 1);
    QVERIFY2(r.ms < 10000, qPrintable(QString::number(r.ms)));
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("unavailable"));
    // the same through `call`
    r = run({Q("--json"), Q("call"), Q("agent.turn"), Q("{\"message\":\"add a marker\"}"), Q("--project"), project_, Q("--llm-endpoint"), kDeadEndpoint});
    QCOMPARE(r.code, 1);
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("unavailable"));
    QVERIFY(r.ms < 10000);
    // and from the environment, as the default
    qputenv("SF_LLM_ENDPOINT", kDeadEndpoint.toUtf8());
    r = run({Q("--json"), Q("agent"), Q("add a marker"), Q("--project"), project_});
    qunsetenv("SF_LLM_ENDPOINT");
    QCOMPARE(r.json().value(Q("error")).toObject().value(Q("code")).toString(), Q("unavailable"));
  }

  void nonAiCommandsNeedNoModel() {
    const QStringList dead{Q("--llm-endpoint"), kDeadEndpoint};
    QCOMPARE(run(withArgs({Q("info"), project_}, dead)).code, 0);
    QCOMPARE(run(withArgs({Q("call"), Q("project.info"), Q("--project"), project_}, dead)).code, 0);
    QCOMPARE(run(withArgs({Q("call"), Q("agent.route"), Q("{\"message\":\"colour grade the clip\"}")}, dead)).code, 0); // stateless, no model call
    QCOMPARE(run(withArgs({Q("commands")}, dead)).code, 0);
  }

  void copilotAndBranchCommandsWorkOverTheCli() {
    Run r = run({Q("--json"), Q("call"), Q("copilot.lint.run"), Q("--project"), project_});
    QCOMPARE(r.code, 0);
    QVERIFY2(r.json().value(Q("ok")).toBool(), qPrintable(r.out));
    QVERIFY(r.json().value(Q("result")).toObject().value(Q("issues")).isArray());
    r = run({Q("--json"), Q("call"), Q("branch.list"), Q("--project"), project_});
    QCOMPARE(r.code, 0);
    const QJsonObject res = r.json().value(Q("result")).toObject();
    QCOMPARE(res.value(Q("active")).toString(), Q("main"));
    QVERIFY(res.value(Q("branches")).toArray().size() >= 1);
    // the copilot's lint and the branch list left no footage catalog behind
    QVERIFY(!QFileInfo::exists(dir_.filePath(Q("p.splitframe-index.sqlite"))));
  }

  void agentTurnIsUndoableWithTheAgentAsActor() {
    Engine e;
    ai::AiServices services(e, mockOptions(script_));
    QVERIFY(e.call(Q("project.open"), {{Q("path"), project_}}).ok);
    QCOMPARE(markersOf(e), 0);
    const Result r = e.call(Q("agent.turn"), {{Q("message"), Q("add a marker at frame 15")}, {Q("mode"), Q("auto")}});
    QVERIFY2(r.ok, qPrintable(r.error.message));
    QCOMPARE(markersOf(e), 1);
    const QJsonArray h = history(e);
    QVERIFY(!h.isEmpty());
    QCOMPARE(h.last().toObject().value(Q("actor")).toString(), Q("agent:editor"));
    QVERIFY(e.call(Q("history.undo")).ok);
    QCOMPARE(markersOf(e), 0);
    QVERIFY(e.call(Q("history.redo")).ok);
    QCOMPARE(markersOf(e), 1);
  }

  void anAttachedLiveProjectGetsTheAgentToo() {
    // what the GUI does: the Editor's own Project and pool wrapped by an Engine
    editor::Project project;
    project.setAutosaveInterval(0);
    project.newProject(Q("Live"), 320, 180, 25);
    editor::MediaPool pool(project);
    Engine e;
    e.attach(&project, &pool);
    ai::AiServices services(e, mockOptions(script_));
    QVERIFY(e.find(Q("agent.turn")) && e.find(Q("copilot.lint.run")) && e.find(Q("branch.list")) && e.find(Q("index.search")));
    QVERIFY(!project.canUndo());
    const Result r = e.call(Q("agent.turn"), {{Q("message"), Q("add a marker at frame 15")}, {Q("mode"), Q("auto")}});
    QVERIFY2(r.ok, qPrintable(r.error.message));
    QCOMPARE(static_cast<int>(project.doc().markers.size()), 1); // the live document, not a copy
    QVERIFY(project.canUndo());
    QCOMPARE(history(e).last().toObject().value(Q("actor")).toString(), Q("agent:editor"));
    project.undo();
    QCOMPARE(static_cast<int>(project.doc().markers.size()), 0);
    const Result b = e.call(Q("branch.list"));
    QVERIFY(b.ok);
    QCOMPARE(b.value.toObject().value(Q("active")).toString(), Q("main"));
  }

  void unreachableEndpointReturnsUnavailableQuickly() {
    ai::AiOptions o;
    o.llm.endpoint = kDeadEndpoint;
    Engine e;
    ai::AiServices services(e, o);
    QVERIFY(e.call(Q("project.open"), {{Q("path"), project_}}).ok);
    QElapsedTimer t;
    t.start();
    const Result r = e.call(Q("agent.turn"), {{Q("message"), Q("add a marker")}});
    QVERIFY(t.elapsed() < 8000);
    QVERIFY(!r.ok);
    QCOMPARE(r.error.code, Q("unavailable"));
    QCOMPARE(markersOf(e), 0);
    // plan mode needs no model and still answers
    QVERIFY(e.call(Q("agent.turn"), {{Q("message"), Q("add a marker and grade it")}, {Q("mode"), Q("plan")}}).ok);
    // a broken mock script is reported the same way
    ai::AiOptions bad = mockOptions(dir_.filePath(Q("nope.json")));
    Engine e2;
    ai::AiServices services2(e2, bad);
    e2.createSession();
    QCOMPARE(e2.call(Q("agent.turn"), {{Q("message"), Q("hi")}}).error.code, Q("unavailable"));
  }

  void theFootageCatalogOpensNextToTheProjectOnlyWhenAnIndexCommandAsks() {
    const QString copy = dir_.filePath(Q("catalog.json"));
    QVERIFY(QFile::copy(project_, copy));
    const QString db = dir_.filePath(Q("catalog.splitframe-index.sqlite"));
    QCOMPARE(ai::AiServices::catalogPathFor(copy, Q("s1")), db);
    {
      Engine e;
      ai::AiServices services(e, mockOptions({}));
      QVERIFY(e.call(Q("project.open"), {{Q("path"), copy}}).ok);
      QVERIFY(e.call(Q("copilot.lint.run")).ok);
      QVERIFY(e.call(Q("branch.list")).ok);
      QVERIFY(!QFileInfo::exists(db)); // nothing started in the background, nothing created for reads
      const Result s = e.call(Q("index.search"), {{Q("query"), Q("hello")}});
      QVERIFY2(s.ok, qPrintable(s.error.message));
      QVERIFY(QFileInfo::exists(db));
    }
    // an unsaved project uses a temp file that goes away with the services
    QString temp;
    {
      Engine e;
      ai::AiServices services(e, mockOptions({}));
      const QString sid = e.createSession();
      temp = ai::AiServices::catalogPathFor({}, sid);
      QVERIFY(e.call(Q("index.search"), {{Q("query"), Q("hello")}}).ok);
      QVERIFY(QFileInfo::exists(temp));
    }
    QVERIFY(!QFileInfo::exists(temp));
  }
};

QTEST_MAIN(TstAiWire)
#include "tst_ai_wire.moc"
