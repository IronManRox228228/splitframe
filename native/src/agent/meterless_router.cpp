#include "agent/meterless_router.h"

namespace sf::meterless_router {

namespace {

const QStringList& classes() {
  static const QStringList c{QStringLiteral("text"), QStringLiteral("code"), QStringLiteral("vision"),
                             QStringLiteral("judge"), QStringLiteral("perception"), QStringLiteral("image"),
                             QStringLiteral("video"), QStringLiteral("speech"), QStringLiteral("transcribe"),
                             QStringLiteral("music"), QStringLiteral("embed"), QStringLiteral("mesh"),
                             QStringLiteral("texture")};
  return c;
}

// SplitFrame's real call sites (the lead's scoping notes plus the official prompt's examples:
// commentary → text, grade → judge, screenshot → vision, still → image, voice → speech,
// theme → music, model → mesh, repaint → texture).
const QStringList& textTasks() {
  static const QStringList t{QStringLiteral("commentary"), QStringLiteral("planning"), QStringLiteral("tool_call"),
                             QStringLiteral("storyboard"), QStringLiteral("generation_prompt"), QStringLiteral("chat"),
                             QStringLiteral("summary")};
  return t;
}

const QStringList& codeTasks() {
  static const QStringList t{QStringLiteral("code")};
  return t;
}

const QStringList& judgeTasks() {
  static const QStringList t{QStringLiteral("peer_review"), QStringLiteral("grade")};
  return t;
}

const QStringList& visionTasks() {
  static const QStringList t{QStringLiteral("screenshot"), QStringLiteral("frame_grab")};
  return t;
}

const QStringList& perceptionTasks() {
  static const QStringList t{QStringLiteral("document"), QStringLiteral("pdf")};
  return t;
}

const QStringList& transcribeTasks() {
  static const QStringList t{QStringLiteral("transcribe")};
  return t;
}

const QStringList& speechTasks() {
  static const QStringList t{QStringLiteral("voice"), QStringLiteral("speech")};
  return t;
}

const QStringList& musicTasks() {
  static const QStringList t{QStringLiteral("theme"), QStringLiteral("music")};
  return t;
}

const QStringList& embedTasks() {
  static const QStringList t{QStringLiteral("embed"), QStringLiteral("search")};
  return t;
}

const QStringList& imageTasks() {
  static const QStringList t{QStringLiteral("still"), QStringLiteral("thumbnail")};
  return t;
}

const QStringList& videoTasks() {
  static const QStringList t{QStringLiteral("video"), QStringLiteral("preview_render")};
  return t;
}

QString classOf(const QString& task) {
  if (textTasks().contains(task)) return QStringLiteral("text");
  if (codeTasks().contains(task)) return QStringLiteral("code");
  if (judgeTasks().contains(task)) return QStringLiteral("judge");
  if (visionTasks().contains(task)) return QStringLiteral("vision");
  if (perceptionTasks().contains(task)) return QStringLiteral("perception");
  if (transcribeTasks().contains(task)) return QStringLiteral("transcribe");
  if (speechTasks().contains(task)) return QStringLiteral("speech");
  if (musicTasks().contains(task)) return QStringLiteral("music");
  if (embedTasks().contains(task)) return QStringLiteral("embed");
  if (imageTasks().contains(task)) return QStringLiteral("image");
  if (videoTasks().contains(task)) return QStringLiteral("video");
  if (task == QStringLiteral("model_3d") || task == QStringLiteral("mesh")) return QStringLiteral("mesh");
  if (task == QStringLiteral("repaint") || task == QStringLiteral("texture") || task == QStringLiteral("paint")) return QStringLiteral("texture");
  return {};
}

// Attachments change the class only when the task's output is text: audio or video → transcribe,
// image → vision, PDF or document → perception. "Make a still from this screenshot" stays image.
QString attachmentClass(const QString& baseClass, const Attachments& attachments) {
  if (baseClass != QStringLiteral("text")) return {};
  for (const QString& a : attachments) {
    if (a == QStringLiteral("image")) return QStringLiteral("vision");
    if (a == QStringLiteral("pdf") || a == QStringLiteral("document")) return QStringLiteral("perception");
    if (a == QStringLiteral("audio") || a == QStringLiteral("video")) return QStringLiteral("transcribe");
  }
  return {};
}

QString acceptFor(const QString& cls) {
  if (cls == QStringLiteral("image")) return QStringLiteral("image");
  if (cls == QStringLiteral("speech") || cls == QStringLiteral("music")) return QStringLiteral("audio");
  if (cls == QStringLiteral("video")) return QStringLiteral("video");
  if (cls == QStringLiteral("mesh") || cls == QStringLiteral("texture")) return QStringLiteral("mesh");
  return {};
}

const QRegularExpression& idParts() {
  // meterless/<class>.<disposition>
  static const QRegularExpression re(QStringLiteral("meterless/([a-z_]+)\\.([a-z]+)"));
  return re;
}

// The downgrade rules (official prompt): fusion on a single-core class (video, music, speech,
// embed, mesh, texture — the fallback list for when GET /v1/meterless/modes does not answer) and
// cluster on vision, perception, transcribe or embed are downgraded to auto before sending, and the
// downgrade is recorded. When the runtime answered /modes, its fusion list replaces the fallback's.
// cluster on a runtime that marks it unavailable is refused, never swapped for auto (error, not a
// downgrade).
QString applyDowngrades(const QString& cls, QString& disposition, QString& downgrade, const RouterOptions& options) {
  const bool runtimeAnswered = options.runtimeModes.has_value();
  bool clusterUnavailable = false;
  QJsonArray runtimeFusionAllowed;
  if (runtimeAnswered) {
    runtimeFusionAllowed = options.runtimeModes->value(QStringLiteral("fusion")).toArray();
    clusterUnavailable = options.runtimeModes->value(QStringLiteral("unavailable")).toArray().contains(QStringLiteral("cluster")) ||
                         (options.runtimeModes->contains(QStringLiteral("cluster")) &&
                          !options.runtimeModes->value(QStringLiteral("cluster")).toBool());
  }

  if (clusterUnavailable && disposition == QStringLiteral("cluster"))
    return QStringLiteral("cluster_unavailable");

  if (disposition == QStringLiteral("fusion")) {
    bool fusionAllowedHere = true;
    if (runtimeAnswered) {
      fusionAllowedHere = false;
      for (const QJsonValue& v : runtimeFusionAllowed)
        if (v.toString() == cls) { fusionAllowedHere = true; break; }
    } else {
      fusionAllowedHere = !fusionSingleCoreClasses().contains(cls);
    }
    if (!fusionAllowedHere) {
      disposition = QStringLiteral("auto");
      downgrade = QStringLiteral("fusion");
    }
  }
  if (disposition == QStringLiteral("cluster") && perceptionLikeClasses().contains(cls)) {
    disposition = QStringLiteral("auto");
    downgrade = QStringLiteral("cluster");
  }
  return {};
}

} // namespace

TaskRoute taskRoute(const QString& task) {
  TaskRoute r;
  r.className = classOf(task);
  if (r.className.isEmpty()) return r;
  r.accept = acceptFor(r.className);
  r.threeD = threeDClasses().contains(r.className);
  // SplitFrame's default disposition is `fast` (the lead's scoping note); the user pick overrides.
  r.disposition = QStringLiteral("fast");
  return r;
}

QJsonObject planRoute(const QString& task, const Attachments& attachments, const RouterOptions& options) {
  TaskRoute base = taskRoute(task);
  QJsonObject out{{QStringLiteral("task"), task}};
  if (base.className.isEmpty()) {
    out.insert(QStringLiteral("error"), QStringLiteral("unknown_task"));
    return out;
  }

  QString disposition = options.disposition.isEmpty() ? QStringLiteral("auto") : options.disposition;
  QString downgrade;
  QString source = QStringLiteral("task");

  // A task never inherits its class from whichever model happens to be selected: the pin can only
  // re-point within the task's own class semantics (a pin REPLACES both id parts for that task).
  QString className = base.className;
  bool threeD = base.threeD;
  QString accept = base.accept;
  if (options.pins && options.pins->contains(task)) {
    const QString pin = options.pins->value(task).toString();
    const auto m = idParts().match(pin);
    if (m.hasMatch()) {
      const QString pinClass = m.captured(1);
      const QString pinDisposition = m.captured(2);
      if (classes().contains(pinClass)) className = pinClass;
      disposition = pinDisposition;
      accept = acceptFor(className);
      threeD = threeDClasses().contains(className);
      source = QStringLiteral("pin");
    }
  }

  // Attachments reclassify only text-output tasks.
  if (source != QStringLiteral("pin")) {
    const QString attach = attachmentClass(className, attachments);
    if (!attach.isEmpty()) {
      className = attach;
      accept = acceptFor(className);
      threeD = threeDClasses().contains(className);
      source = QStringLiteral("attachment");
    }
  }

  if (const QString refused = applyDowngrades(className, disposition, downgrade, options); !refused.isEmpty()) {
    out.insert(QStringLiteral("error"), refused);
    return out;
  }

  out.insert(QStringLiteral("id"), QStringLiteral("meterless/%1.%2").arg(className, disposition));
  out.insert(QStringLiteral("class"), className);
  out.insert(QStringLiteral("disposition"), disposition);
  out.insert(QStringLiteral("route"), threeD ? QStringLiteral("3d") : QStringLiteral("chat"));
  out.insert(QStringLiteral("source"), source);
  out.insert(QStringLiteral("accept"), accept); // meterless-accept value for this request ("" = no header)
  if (!downgrade.isEmpty()) out.insert(QStringLiteral("downgrade"), downgrade);
  return out;
}

QJsonArray routingTable() {
  // One read-only row per call-site task, computed by the same rules the router enforces.
  static const QStringList tasks{QStringLiteral("commentary"), QStringLiteral("planning"), QStringLiteral("tool_call"),
                                 QStringLiteral("storyboard"), QStringLiteral("generation_prompt"), QStringLiteral("chat"),
                                 QStringLiteral("summary"), QStringLiteral("code"), QStringLiteral("peer_review"),
                                 QStringLiteral("grade"), QStringLiteral("screenshot"), QStringLiteral("frame_grab"),
                                 QStringLiteral("document"), QStringLiteral("pdf"), QStringLiteral("transcribe"),
                                 QStringLiteral("voice"), QStringLiteral("speech"), QStringLiteral("theme"),
                                 QStringLiteral("music"), QStringLiteral("embed"), QStringLiteral("search"),
                                 QStringLiteral("still"), QStringLiteral("thumbnail"), QStringLiteral("video"),
                                 QStringLiteral("preview_render"), QStringLiteral("model_3d"), QStringLiteral("mesh"),
                                 QStringLiteral("repaint"), QStringLiteral("texture"), QStringLiteral("paint")};
  QJsonArray rows;
  for (const QString& t : tasks) {
    const TaskRoute r = taskRoute(t);
    if (r.className.isEmpty()) continue;
    rows.append(QJsonObject{{QStringLiteral("task"), t},
                            {QStringLiteral("class"), r.className},
                            {QStringLiteral("route"), threeDClasses().contains(r.className) ? QStringLiteral("3d") : QStringLiteral("chat")},
                            {QStringLiteral("meterless-accept"), r.accept}});
  }
  return rows;
}

} // namespace sf::meterless_router
