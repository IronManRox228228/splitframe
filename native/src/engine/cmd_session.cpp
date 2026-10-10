// Session commands: project.new/open/save/saveAs/close/info, project.settings.set, project.color.set, sessions.list.

#include "engine/cmd_util.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace sf::engine {

namespace {

QJsonObject projectInfo(const Session& s) {
  const editor::Project& p = *s.project;
  const TimelineDoc& d = p.doc();
  QJsonObject o{{q("session"), s.id},
                {q("name"), d.project.name},
                {q("id"), d.project.id},
                {q("path"), p.path()},
                {q("dirty"), p.isDirty()},
                {q("fps"), static_cast<double>(d.project.fps)},
                {q("width"), static_cast<double>(d.project.width)},
                {q("height"), static_cast<double>(d.project.height)},
                {q("durationFrames"), static_cast<double>(docDurationFrames(d))},
                {q("durationSec"), framesToSeconds(docDurationFrames(d), static_cast<double>(d.project.fps))},
                {q("tracks"), static_cast<int>(d.tracks.size())},
                {q("items"), static_cast<int>(d.items.size())},
                {q("markers"), static_cast<int>(d.markers.size())},
                {q("assets"), static_cast<int>(p.assets().size())},
                {q("canUndo"), p.canUndo()},
                {q("canRedo"), p.canRedo()},
                {q("undoLabel"), p.undoLabel()},
                {q("hasMixer"), d.mixer.has_value()},
                {q("external"), s.external()}};
  const QJsonObject proj = toJson(d.project);
  if (proj.contains(q("colorManagement"))) o.insert(q("colorManagement"), proj.value(q("colorManagement")));
  return o;
}

inline int even(double n) { return std::max(2, static_cast<int>(std::lround(n / 2.0)) * 2); }

std::optional<std::pair<int, int>> canvasForAspect(const QString& aspect, int shortSide) {
  static const std::map<QString, std::pair<int, int>> aspects{{q("16:9"), {16, 9}}, {q("9:16"), {9, 16}}, {q("1:1"), {1, 1}}, {q("4:5"), {4, 5}},
                                                              {q("5:4"), {5, 4}},   {q("4:3"), {4, 3}},   {q("3:4"), {3, 4}}, {q("21:9"), {21, 9}}};
  const auto it = aspects.find(aspect);
  if (it == aspects.end()) return std::nullopt;
  const auto [aw, ah] = it->second;
  if (aw >= ah) return std::make_pair(even(static_cast<double>(shortSide) * aw / ah), even(shortSide));
  return std::make_pair(even(shortSide), even(static_cast<double>(shortSide) * ah / aw));
}

// Keeps items in place relative to the canvas: offsets (and their keyframes) scale with it.
void relayout(const TimelineDoc& d, int w, int h, QJsonArray* ops) {
  const double rx = static_cast<double>(w) / static_cast<double>(d.project.width);
  const double ry = static_cast<double>(h) / static_cast<double>(d.project.height);
  for (const Item& it : d.items) {
    QJsonObject patch;
    if (it.transform.x != 0 || it.transform.y != 0)
      patch.insert(q("transform"), QJsonObject{{q("x"), static_cast<double>(jsRound(it.transform.x * rx))}, {q("y"), static_cast<double>(jsRound(it.transform.y * ry))}});
    const bool kx = it.keyframes.count(q("transform.x")) > 0, ky = it.keyframes.count(q("transform.y")) > 0;
    if (kx || ky) {
      KeyframeMap kf = it.keyframes;
      if (kx) for (Keyframe& k : kf[q("transform.x")]) k.value *= rx;
      if (ky) for (Keyframe& k : kf[q("transform.y")]) k.value *= ry;
      patch.insert(q("keyframes"), toJson(kf));
    }
    if (!patch.isEmpty()) ops->append(QJsonObject{{q("type"), q("item.update")}, {q("itemId"), it.id}, {q("patch"), patch}});
  }
}

void loadAssets(Session& s) {
  if (s.pool) s.pool->projectLoaded();
}

} // namespace

void registerSessionCommands(Engine& e) {
  e.add(spec("sessions.list", "", "engine", false, QStringLiteral("List open project sessions."), js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               QJsonArray a;
               for (const QString& id : c.engine->sessionIds()) a.append(projectInfo(*c.engine->session(id)));
               return Result::success(QJsonObject{{q("sessions"), a}, {q("current"), c.engine->currentSession()}});
             },
             false));

  {
    QJsonObject schema = js::obj({{q("name"), js::str(QStringLiteral("Project name"))},
                                  {q("width"), js::integer(QStringLiteral("Canvas width"), 64, 7680)},
                                  {q("height"), js::integer(QStringLiteral("Canvas height"), 64, 7680)},
                                  {q("fps"), js::integer(QStringLiteral("Frames per second"), 1, 120)},
                                  {q("session"), js::str(QStringLiteral("Reset this session in place instead of creating one"))}});
    e.add(spec("project.new", "", "engine", false, QStringLiteral("Create an empty project (a new session unless `session` names one to reset)."), schema,
               [](CallContext& c, const QJsonObject& a) {
                 Engine& eng = *c.engine;
                 Session* s = a.contains(q("session")) ? eng.session(a.value(q("session")).toString()) : nullptr;
                 if (a.contains(q("session")) && !s) return Result::failure(q("no_session"), QStringLiteral("No session \"%1\".").arg(a.value(q("session")).toString()));
                 if (!s) s = eng.session(eng.createSession());
                 s->project->newProject(a.value(q("name")).toString(QStringLiteral("Untitled")), a.value(q("width")).toInt(1920), a.value(q("height")).toInt(1080),
                                        a.value(q("fps")).toInt(30));
                 return Result::success(projectInfo(*s));
               },
               false));
  }
  {
    QJsonObject schema = js::obj({{q("path"), js::str(QStringLiteral("Project .json file"))},
                                  {q("session"), js::str(QStringLiteral("Open into this existing session instead of a new one"))}},
                                 {q("path")});
    e.add(spec("project.open", "", "engine", false, QStringLiteral("Open a project file (a new session unless `session` names one to replace)."), schema,
               [](CallContext& c, const QJsonObject& a) {
                 Engine& eng = *c.engine;
                 const QString path = a.value(q("path")).toString();
                 if (!QFileInfo::exists(path)) return Result::failure(q("not_found"), QStringLiteral("No such file: %1").arg(path));
                 Session* s = a.contains(q("session")) ? eng.session(a.value(q("session")).toString()) : nullptr;
                 if (a.contains(q("session")) && !s) return Result::failure(q("no_session"), QStringLiteral("No session \"%1\".").arg(a.value(q("session")).toString()));
                 const bool fresh = !s;
                 if (fresh) s = eng.session(eng.createSession());
                 QString err;
                 if (!s->project->open(path, &err)) {
                   if (fresh) eng.closeSession(s->id);
                   return Result::failure(q("io_error"), err);
                 }
                 loadAssets(*s);
                 return Result::success(projectInfo(*s));
               },
               false));
  }
  e.add(spec("project.save", "", "engine", false, QStringLiteral("Save the project to its file (project.saveAs for a project that has none)."), js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               if (c.session->project->path().isEmpty()) return Result::failure(q("invalid_params"), QStringLiteral("The project has no file yet: use project.saveAs."));
               QString err;
               if (!c.session->project->save(&err)) return Result::failure(q("io_error"), err);
               return Result::success(projectInfo(*c.session));
             }));
  e.add(spec("project.saveAs", "", "engine", false, QStringLiteral("Save the project to a new file and make that its file."), js::obj({{q("path"), js::str(QStringLiteral("Target .json path"))}}, {q("path")}),
             [](CallContext& c, const QJsonObject& a) {
               QString err;
               if (!c.session->project->saveAs(a.value(q("path")).toString(), &err)) return Result::failure(q("io_error"), err);
               return Result::success(projectInfo(*c.session));
             }));
  e.add(spec("project.close", "", "engine", false, QStringLiteral("Close a session. Refuses a project with unsaved changes unless discard is true."),
             js::obj({{q("discard"), js::boolean(QStringLiteral("Close even with unsaved changes"))}}),
             [](CallContext& c, const QJsonObject& a) {
               if (c.session->external()) return Result::failure(q("unsupported"), QStringLiteral("The GUI's own project cannot be closed through the API."));
               if (c.session->project->isDirty() && !a.value(q("discard")).toBool())
                 return Result::failure(q("busy"), QStringLiteral("The project has unsaved changes: save it or pass discard=true."));
               const QString id = c.session->id;
               c.engine->closeSession(id);
               return Result::success(QJsonObject{{q("closed"), id}});
             }));
  e.add(spec("project.info", "getEditorContext", "engine", false, QStringLiteral("Project summary: name, path, dirty, canvas, fps, duration, counts, undo state, colour management."), js::obj({}),
             [](CallContext& c, const QJsonObject&) { return Result::success(projectInfo(*c.session)); }));

  e.add(spec("project.settings.set", "setProjectSettings", "editor", true,
             QStringLiteral("Change the canvas and/or frame rate (one undo step). Canvas: `aspect` (\"9:16\", \"16:9\", \"1:1\", \"4:5\", \"5:4\", \"4:3\", \"3:4\", \"21:9\") with optional "
                            "shortSidePx (default 1080), or width+height. Items keep their place (offsets scale); an fps change keeps every cut at the same time in seconds."),
             js::obj({{q("aspect"), js::choice({q("16:9"), q("9:16"), q("1:1"), q("4:5"), q("5:4"), q("4:3"), q("3:4"), q("21:9")})},
                      {q("shortSidePx"), js::integer(QStringLiteral("Short side in pixels when using aspect"), 240, 4320)},
                      {q("width"), js::integer(QStringLiteral("Explicit canvas width (with height)"), 64, 7680)},
                      {q("height"), js::integer(QStringLiteral("Explicit canvas height (with width)"), 64, 7680)},
                      {q("fps"), js::integer(QStringLiteral("Frames per second"), 1, 120)},
                      {q("name"), js::str(QStringLiteral("Rename the project"))}}),
             [](CallContext& c, const QJsonObject& a) {
               const TimelineDoc& d = c.session->project->doc();
               std::optional<std::pair<int, int>> target;
               if (a.contains(q("width")) || a.contains(q("height"))) {
                 if (!a.contains(q("width")) || !a.contains(q("height"))) return invalid(QStringLiteral("Pass both width and height, or use aspect."));
                 target = std::make_pair(even(a.value(q("width")).toDouble()), even(a.value(q("height")).toDouble()));
               } else if (a.contains(q("aspect"))) {
                 target = canvasForAspect(a.value(q("aspect")).toString(), a.value(q("shortSidePx")).toInt(1080));
               }
               QJsonArray ops;
               QJsonArray changed;
               if (target && (target->first != d.project.width || target->second != d.project.height)) {
                 ops.append(QJsonObject{{q("type"), q("project.setCanvas")}, {q("width"), target->first}, {q("height"), target->second}});
                 relayout(d, target->first, target->second, &ops);
                 changed.append(QStringLiteral("canvas %1x%2 -> %3x%4").arg(d.project.width).arg(d.project.height).arg(target->first).arg(target->second));
               }
               if (a.contains(q("fps")) && a.value(q("fps")).toInteger() != d.project.fps) {
                 ops.append(QJsonObject{{q("type"), q("project.setFps")}, {q("fps"), a.value(q("fps"))}});
                 changed.append(QStringLiteral("fps %1 -> %2").arg(d.project.fps).arg(a.value(q("fps")).toInteger()));
               }
               if (a.contains(q("name")) && a.value(q("name")).toString() != d.project.name) {
                 ops.append(QJsonObject{{q("type"), q("project.rename")}, {q("name"), a.value(q("name"))}});
                 changed.append(QStringLiteral("name"));
               }
               if (ops.isEmpty()) {
                 if (!target && !a.contains(q("fps")) && !a.contains(q("name"))) return invalid(QStringLiteral("Nothing to change: pass aspect, width+height, fps and/or name."));
                 return Result::success(QJsonObject{{q("applied"), false}, {q("note"), QStringLiteral("Project already has these settings.")}});
               }
               return applyToSession(c, parseOps(ops), QStringLiteral("setProjectSettings"), {{q("changed"), changed}});
             }));

  e.add(spec("project.color.set", "", "colour", true,
             QStringLiteral("Set the project's colour management (native-only block): workingSpace (OpenColorIO scene-linear space), outputSpace (\"srgb\", \"rec709\", \"rec2100pq\", "
                            "\"rec2100hlg\" or a display-referred OCIO space), displayView (\"Display/View\"), blendSpace (\"linear\" | \"display\"). Fields given replace the block's; "
                            "`clear` removes it. Not undoable (core has no op for it)."),
             js::obj({{q("workingSpace"), js::str()}, {q("outputSpace"), js::str()}, {q("displayView"), js::str()},
                      {q("blendSpace"), js::choice({q("linear"), q("display")})}, {q("clear"), js::boolean(QStringLiteral("Remove the block"))}}),
             [](CallContext& c, const QJsonObject& a) {
               editor::Project& p = *c.session->project;
               if (a.value(q("clear")).toBool()) {
                 p.setColorManagement(std::nullopt);
                 return Result::success(projectInfo(*c.session));
               }
               QJsonObject cm = toJson(p.doc().project).value(q("colorManagement")).toObject();
               for (const char* k : {"workingSpace", "outputSpace", "displayView", "blendSpace"})
                 if (a.contains(q(k))) cm.insert(q(k), a.value(q(k)));
               if (cm.isEmpty()) return invalid(QStringLiteral("Nothing to set: pass workingSpace, outputSpace, displayView, blendSpace or clear."));
               QJsonObject proj = toJson(p.doc().project);
               proj.insert(q("colorManagement"), cm);
               const Project parsed = parseProject(Rd(proj, QStringLiteral("project")));
               p.setColorManagement(parsed.colorManagement);
               return Result::success(projectInfo(*c.session));
             }));
}

} // namespace sf::engine
