// splitframe-cli: the engine from a terminal. Console program, no window, no QML.
//
//   splitframe-cli [--json] info <project>
//   splitframe-cli [--json] timeline <project> [--detail] [--track <id>]... [--from <frame>] [--to <frame>]
//   splitframe-cli [--json] apply <project> <ops.json> [--save] [--out <file>] [--label <text>] [--origin <who>]
//   splitframe-cli [--json] render-frame <project> --frame <n> --out <png> [--max-width <px>]
//   splitframe-cli export <project> --out <file> [export options of splitframe.exe --export]   (key=value progress lines)
//   splitframe-cli [--json] call <command> [<json args>] [--project <file>]
//   splitframe-cli [--json] commands [--domain editor|colour|audio|engine]
//   splitframe-cli serve [<project>] [--port <n>] [--token-file <path>] [--stdio]
//   splitframe-cli mcp [<project>]
//
// Output: human text, or with --json one JSON document {"ok":true,"result":...} / {"ok":false,"error":{code,message,details?}}.
// Exit codes: 0 ok, 1 the command failed (see error.code), 2 bad usage, 3 export cancelled, 4 the project can't be opened / read.

#include "engine/engine.h"
#include "engine/headless.h"
#include "export/export_cli.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <QMap>
#include <QSet>

#include <cstdio>
#include <functional>

using namespace sf;
using namespace sf::engine;

namespace {

QString Q(const char* s) { return QString::fromLatin1(s); }

void out(const QString& text) {
  const QByteArray b = text.toUtf8() + '\n';
  std::fwrite(b.constData(), 1, static_cast<size_t>(b.size()), stdout);
  std::fflush(stdout);
}
void err(const QString& text) {
  const QByteArray b = text.toUtf8() + '\n';
  std::fwrite(b.constData(), 1, static_cast<size_t>(b.size()), stderr);
}
QString pretty(const QJsonValue& v) {
  if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented)).trimmed();
  if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)).trimmed();
  return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
}

int usage(const QString& message) {
  err(QStringLiteral("error: %1\nusage: splitframe-cli [--json] <info|timeline|apply|render-frame|export|call|commands|serve|mcp> ... (see the header of cli/main.cpp)").arg(message));
  return 2;
}

struct Args {
  QStringList positional;
  QMap<QString, QStringList> values; // --key value (repeatable)
  QSet<QString> flags;
  QString error;
  QString value(const QString& k, const QString& def = {}) const { return values.contains(k) ? values.value(k).last() : def; }
  bool has(const QString& k) const { return flags.contains(k) || values.contains(k); }
};

Args parse(const QStringList& in, const QSet<QString>& valued) {
  Args a;
  for (int i = 0; i < in.size(); ++i) {
    const QString s = in[i];
    if (s.startsWith(QStringLiteral("--")) && s.size() > 2) {
      const QString key = s.mid(2);
      if (valued.contains(key)) {
        if (i + 1 >= in.size()) {
          a.error = QStringLiteral("--%1 needs a value").arg(key);
          return a;
        }
        a.values[key] << in[++i];
      } else {
        a.flags.insert(key);
      }
    } else {
      a.positional << s;
    }
  }
  return a;
}

// Prints a Result and picks the exit code.
int finish(const Result& r, bool json, const std::function<QString(const QJsonObject&)>& human = {}) {
  if (json) {
    out(QString::fromUtf8(QJsonDocument(r.toJson()).toJson(QJsonDocument::Indented)).trimmed());
  } else if (r.ok) {
    out(human ? human(r.value.toObject()) : pretty(r.value));
  } else {
    err(QStringLiteral("error [%1]: %2").arg(r.error.code, r.error.message));
    if (!r.error.details.isUndefined()) err(pretty(r.error.details));
  }
  return r.ok ? 0 : 1;
}

bool openProject(Engine& e, const QString& path, bool json, int* exit) {
  const Result r = e.call(Q("project.open"), {{Q("path"), path}});
  if (r.ok) return true;
  if (json) out(QString::fromUtf8(QJsonDocument(r.toJson()).toJson(QJsonDocument::Indented)).trimmed());
  else err(QStringLiteral("error: %1").arg(r.error.message));
  *exit = 4;
  return false;
}

QString timelineText(const QJsonObject& t) {
  const QJsonObject p = t.value(Q("project")).toObject();
  QStringList lines;
  lines << QStringLiteral("%1  %2x%3 @ %4 fps, %5 frames").arg(p.value(Q("name")).toString()).arg(p.value(Q("width")).toInt()).arg(p.value(Q("height")).toInt()).arg(p.value(Q("fps")).toInt()).arg(p.value(Q("durationFrames")).toInt());
  for (const QJsonValue& tv : t.value(Q("tracks")).toArray()) {
    const QJsonObject tr = tv.toObject();
    lines << QStringLiteral("[%1] %2 (%3)%4").arg(tr.value(Q("id")).toString(), tr.value(Q("name")).toString(), tr.value(Q("kind")).toString(), tr.value(Q("locked")).toBool() ? QStringLiteral(" locked") : QString());
    for (const QJsonValue& iv : tr.value(Q("items")).toArray()) {
      const QJsonObject it = iv.toObject();
      lines << QStringLiteral("  %1  %2  %3 +%4  %5").arg(it.value(Q("id")).toString(), it.value(Q("type")).toString()).arg(it.value(Q("start")).toInt()).arg(it.value(Q("dur")).toInt())
                   .arg(it.value(Q("name")).toString(it.value(Q("text")).toString()));
    }
  }
  for (const QJsonValue& mv : t.value(Q("markers")).toArray()) lines << QStringLiteral("marker %1 @ %2").arg(mv.toObject().value(Q("label")).toString()).arg(mv.toObject().value(Q("frame")).toInt());
  return lines.join(QLatin1Char('\n'));
}

} // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(Q("SplitFrame"));
  QGuiApplication::setOrganizationName(Q("SplitFrame"));
  QGuiApplication::setApplicationVersion(Q("0.1.0"));
  QStringList args = QCoreApplication::arguments();
  args.removeFirst();
  bool json = false;
  json = args.removeAll(Q("--json")) > 0;
  if (args.isEmpty() || args.first() == Q("--help") || args.first() == Q("-h") || args.first() == Q("help")) {
    out(QStringLiteral("splitframe-cli [--json] <command>\n  info <project> | timeline <project> | apply <project> <ops.json> | render-frame <project> | export <project> | call <command> [json] | commands | serve | mcp\n"
                       "Exit codes: 0 ok, 1 command failed, 2 usage, 3 export cancelled, 4 project can't be opened."));
    return args.isEmpty() ? 2 : 0;
  }
  if (args.first() == Q("--version")) {
    out(QStringLiteral("splitframe-cli %1").arg(QGuiApplication::applicationVersion()));
    return 0;
  }
  const QString cmd = args.takeFirst();

  if (cmd == Q("export")) {
    if (args.isEmpty()) return usage(QStringLiteral("export needs a project file"));
    QStringList fwd{QStringLiteral("splitframe-cli"), Q("--export"), args.takeFirst()};
    fwd += args;
    return sf::xport::runExportCli(fwd);
  }
  if (cmd == Q("mcp")) return runMcp(args.value(0));
  if (cmd == Q("serve")) {
    const Args a = parse(args, {Q("port"), Q("token-file")});
    if (!a.error.isEmpty()) return usage(a.error);
    ServeOptions so;
    so.project = a.positional.value(0);
    so.port = static_cast<quint16>(a.value(Q("port"), Q("0")).toUInt());
    so.tokenFile = a.value(Q("token-file"));
    so.stdio = a.has(Q("stdio"));
    return runServe(so);
  }

  Engine engine;
  int code = 0;
  if (cmd == Q("commands")) {
    const Args a = parse(args, {Q("domain")});
    if (!a.error.isEmpty()) return usage(a.error);
    const QJsonArray list = engine.describe(a.value(Q("domain")));
    if (json) {
      out(QString::fromUtf8(QJsonDocument(QJsonObject{{Q("ok"), true}, {Q("result"), QJsonObject{{Q("commands"), list}}}}).toJson(QJsonDocument::Indented)).trimmed());
    } else {
      for (const QJsonValue& v : list) {
        const QJsonObject c = v.toObject();
        out(QStringLiteral("%1  [%2%3]  %4").arg(c.value(Q("name")).toString(), c.value(Q("domain")).toString(), c.value(Q("mutates")).toBool() ? QStringLiteral(", mutates") : QString(), c.value(Q("description")).toString().left(90)));
      }
    }
    return 0;
  }

  if (cmd == Q("call")) {
    const Args a = parse(args, {Q("project")});
    if (!a.error.isEmpty()) return usage(a.error);
    if (a.positional.isEmpty()) return usage(QStringLiteral("call needs a command name"));
    QJsonObject params;
    if (a.positional.size() > 1) {
      QJsonParseError pe;
      const QJsonDocument d = QJsonDocument::fromJson(a.positional.at(1).toUtf8(), &pe);
      if (!d.isObject()) return usage(QStringLiteral("the arguments must be a JSON object: %1").arg(pe.errorString()));
      params = d.object();
    }
    const CommandSpec* spec = engine.find(a.positional.first());
    if (a.has(Q("project"))) {
      if (!openProject(engine, a.value(Q("project")), json, &code)) return code;
    } else if (spec && spec->needsSession) {
      engine.createSession();
    }
    return finish(engine.call(a.positional.first(), params), json);
  }

  // the remaining subcommands take a project
  const Args a = parse(args, {Q("track"), Q("from"), Q("to"), Q("out"), Q("label"), Q("origin"), Q("frame"), Q("max-width")});
  if (!a.error.isEmpty()) return usage(a.error);
  if (a.positional.isEmpty()) return usage(QStringLiteral("%1 needs a project file").arg(cmd));
  if (cmd != Q("info") && cmd != Q("timeline") && cmd != Q("apply") && cmd != Q("render-frame")) return usage(QStringLiteral("unknown command \"%1\"").arg(cmd));
  if (!openProject(engine, a.positional.first(), json, &code)) return code;

  if (cmd == Q("info")) {
    return finish(engine.call(Q("project.info")), json, [](const QJsonObject& o) {
      return QStringLiteral("%1\n  path: %2\n  canvas: %3x%4 @ %5 fps\n  duration: %6 s (%7 frames)\n  tracks: %8, items: %9, markers: %10, assets: %11")
          .arg(o.value(Q("name")).toString(), o.value(Q("path")).toString()).arg(o.value(Q("width")).toInt()).arg(o.value(Q("height")).toInt()).arg(o.value(Q("fps")).toInt())
          .arg(o.value(Q("durationSec")).toDouble(), 0, 'f', 2).arg(o.value(Q("durationFrames")).toInt()).arg(o.value(Q("tracks")).toInt()).arg(o.value(Q("items")).toInt())
          .arg(o.value(Q("markers")).toInt()).arg(o.value(Q("assets")).toInt());
    });
  }
  if (cmd == Q("timeline")) {
    QJsonObject p{{Q("mode"), a.has(Q("detail")) ? Q("detail") : Q("summary")}};
    if (a.values.contains(Q("track"))) p.insert(Q("trackIds"), QJsonArray::fromStringList(a.values.value(Q("track"))));
    if (a.has(Q("from"))) p.insert(Q("startFrame"), a.value(Q("from")).toLongLong());
    if (a.has(Q("to"))) p.insert(Q("endFrame"), a.value(Q("to")).toLongLong());
    return finish(engine.call(Q("timeline.get"), p), json, timelineText);
  }
  if (cmd == Q("apply")) {
    if (a.positional.size() < 2) return usage(QStringLiteral("apply needs <project> <ops.json>"));
    QFile f(a.positional.at(1));
    if (!f.open(QIODevice::ReadOnly)) {
      err(QStringLiteral("error: can't read %1").arg(a.positional.at(1)));
      return 4;
    }
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    const QJsonArray ops = d.isArray() ? d.array() : d.object().value(Q("ops")).toArray();
    if (d.isNull() || ops.isEmpty()) return usage(QStringLiteral("%1 must hold a JSON array of ops (or {\"ops\": [...]})%2").arg(a.positional.at(1), d.isNull() ? QStringLiteral(": ") + pe.errorString() : QString()));
    Result r = engine.call(Q("ops.apply"), {{Q("ops"), ops}, {Q("label"), a.value(Q("label"), Q("cli apply"))}, {Q("origin"), a.value(Q("origin"), Q("cli"))}});
    bool saved = false;
    if (r.ok && (a.has(Q("save")) || a.has(Q("out")))) {
      const Result s = a.has(Q("out")) ? engine.call(Q("project.saveAs"), {{Q("path"), a.value(Q("out"))}}) : engine.call(Q("project.save"));
      if (!s.ok) return finish(s, json);
      saved = true;
    }
    if (r.ok) {
      QJsonObject o = r.value.toObject();
      o.insert(Q("saved"), saved);
      r = Result::success(o);
    }
    return finish(r, json, [](const QJsonObject& o) {
      return QStringLiteral("applied %1 op(s)%2").arg(o.value(Q("applied")).toInt()).arg(o.value(Q("saved")).toBool() ? QStringLiteral(", saved") : QStringLiteral(" (not saved: pass --save or --out)"));
    });
  }
  // render-frame
  if (!a.has(Q("out"))) return usage(QStringLiteral("render-frame needs --out <png>"));
  QJsonObject p{{Q("path"), a.value(Q("out"))}, {Q("frame"), a.value(Q("frame"), Q("0")).toLongLong()}};
  if (a.has(Q("max-width"))) p.insert(Q("maxWidth"), a.value(Q("max-width")).toInt());
  return finish(engine.call(Q("frame.render"), p), json, [](const QJsonObject& o) { return QStringLiteral("wrote %1 (%2x%3)").arg(o.value(Q("path")).toString()).arg(o.value(Q("width")).toInt()).arg(o.value(Q("height")).toInt()); });
}
