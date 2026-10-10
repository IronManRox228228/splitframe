// Media commands: media.import / remove / relink, proxies.request / status.

#include "engine/cmd_util.h"

#include <QDir>
#include <QFileInfo>

namespace sf::engine {

namespace {

QJsonObject proxyRow(const Session& s, const Asset& a) {
  QJsonObject o{{q("assetId"), a.id}, {q("name"), a.originalName}, {q("state"), s.pool->proxyState(a.id)}};
  const double prog = s.pool->proxyProgress(a.id);
  if (prog > 0) o.insert(q("progress"), prog);
  const QString path = s.pool->proxyPath(a.id);
  if (!path.isEmpty()) o.insert(q("path"), QDir::toNativeSeparators(path));
  return o;
}

} // namespace

void registerMediaCommands(Engine& e) {
  e.add(spec("media.import", "", "engine", true,
             QStringLiteral("Import media files as assets (probe + thumbnails / waveform run in the background). Waits until analysis finishes unless wait=false. Files already in the project are reported "
                            "as duplicates; files that are not media as skipped. Assets are library state: not undoable."),
             js::obj({{q("paths"), [] { QJsonObject o = js::array(js::str()); o.insert(q("minItems"), 1); return o; }()}, {q("wait"), js::boolean(QStringLiteral("Wait for analysis (default true)"))},
                      {q("timeoutMs"), js::integer(QStringLiteral("Wait limit (default 60000)"), 100, 600000)}},
                     {q("paths")}),
             [](CallContext& c, const QJsonObject& a) {
               QStringList paths, missing;
               for (const QJsonValue& v : a.value(q("paths")).toArray()) {
                 const QString p = v.toString();
                 if (QFileInfo::exists(p)) paths << QFileInfo(p).absoluteFilePath();
                 else missing << p;
               }
               editor::ImportSummary sum;
               if (!paths.isEmpty()) sum = c.session->pool->import(paths);
               const bool wait = a.value(q("wait")).toBool(true);
               bool idle = true;
               if (wait) idle = c.session->pool->waitForIdle(a.value(q("timeoutMs")).toInt(60000));
               QJsonArray assets;
               for (const QString& id : sum.added + sum.duplicates)
                 if (const Asset* as = c.session->project->asset(id)) assets.append(assetBrief(*as, c.session->pool));
               return Result::success(QJsonObject{{q("added"), QJsonArray::fromStringList(sum.added)}, {q("duplicates"), QJsonArray::fromStringList(sum.duplicates)},
                                                  {q("skipped"), QJsonArray::fromStringList(sum.skipped)}, {q("missing"), QJsonArray::fromStringList(missing)},
                                                  {q("assets"), assets}, {q("analysisComplete"), idle}});
             }));

  e.add(spec("media.remove", "", "engine", true, QStringLiteral("Remove an asset. Refuses while timeline items use it unless removeClips=true (those items go first, as one undo group)."),
             js::obj({{q("assetId"), js::str()}, {q("removeClips"), js::boolean()}}, {q("assetId")}),
             [](CallContext& c, const QJsonObject& a) {
               const QString id = a.value(q("assetId")).toString();
               if (!c.session->project->asset(id)) return notFound(QStringLiteral("Asset %1 not found.").arg(id), QStringLiteral("Call assets.list."));
               const int uses = c.session->pool->usesOf(id);
               const bool removeClips = a.value(q("removeClips")).toBool();
               if (uses > 0 && !removeClips)
                 return Result::failure(q("edit_rejected"), QStringLiteral("Asset %1 is used by %2 timeline item(s): pass removeClips=true to remove them too.").arg(id).arg(uses));
               if (!c.session->pool->removeAsset(id, removeClips)) return Result::failure(q("edit_rejected"), c.session->project->lastError().isEmpty() ? QStringLiteral("Could not remove %1.").arg(id) : c.session->project->lastError());
               return Result::success(QJsonObject{{q("removed"), id}, {q("clipsRemoved"), removeClips ? uses : 0}});
             }));

  e.add(spec("media.relink", "", "engine", true, QStringLiteral("Point an asset (e.g. a missing one) at a file; it is analysed again."),
             js::obj({{q("assetId"), js::str()}, {q("path"), js::str()}}, {q("assetId"), q("path")}),
             [](CallContext& c, const QJsonObject& a) {
               const Asset* cur = c.session->project->asset(a.value(q("assetId")).toString());
               if (!cur) return notFound(QStringLiteral("Asset %1 not found.").arg(a.value(q("assetId")).toString()), QStringLiteral("Call assets.list."));
               const QString path = a.value(q("path")).toString();
               if (!QFileInfo::exists(path)) return Result::failure(q("not_found"), QStringLiteral("No such file: %1").arg(path));
               Asset next = *cur;
               next.path = QFileInfo(path).absoluteFilePath();
               next.status = AssetStatus::Missing;
               c.session->project->updateAsset(next);
               c.session->pool->refreshAvailability();
               c.session->pool->waitForIdle(60000);
               return Result::success(assetBrief(*c.session->project->asset(next.id), c.session->pool));
             }));

  e.add(spec("proxies.request", "", "engine", true,
             QStringLiteral("Start proxy generation (all-intra H.264, 960 px) for one video asset, or all of them. Proxies speed up preview scrubbing; export never uses them. wait=true blocks until done."),
             js::obj({{q("assetId"), js::str()}, {q("all"), js::boolean()}, {q("wait"), js::boolean()}, {q("timeoutMs"), js::integer(QString(), 100, 1800000)}}),
             [](CallContext& c, const QJsonObject& a) {
               if (a.contains(q("assetId"))) {
                 if (!c.session->project->asset(a.value(q("assetId")).toString())) return notFound(QStringLiteral("Asset %1 not found.").arg(a.value(q("assetId")).toString()));
                 c.session->pool->requestProxy(a.value(q("assetId")).toString());
               } else if (a.value(q("all")).toBool()) {
                 c.session->pool->requestAllProxies();
               } else {
                 return invalid(QStringLiteral("Pass assetId or all=true."));
               }
               bool done = true;
               if (a.value(q("wait")).toBool()) done = c.session->pool->waitForProxies(a.value(q("timeoutMs")).toInt(600000));
               QJsonArray rows;
               for (const Asset& as : c.session->project->assets())
                 if (as.kind == AssetKind::Video) rows.append(proxyRow(*c.session, as));
               return Result::success(QJsonObject{{q("proxies"), rows}, {q("pending"), c.session->pool->proxyPending()}, {q("complete"), done}});
             }));

  e.add(spec("proxies.status", "", "engine", false, QStringLiteral("Proxy state per video asset: none | queued | running (with progress) | ready (with path) | unneeded | failed."), js::obj({}),
             [](CallContext& c, const QJsonObject&) {
               QJsonArray rows;
               for (const Asset& as : c.session->project->assets())
                 if (as.kind == AssetKind::Video) rows.append(proxyRow(*c.session, as));
               return Result::success(QJsonObject{{q("proxies"), rows}, {q("pending"), c.session->pool->proxyPending()}});
             }));
}

} // namespace sf::engine
