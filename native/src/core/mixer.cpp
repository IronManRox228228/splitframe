#include "core/schema.h"

#include <QJsonArray>

// JSON for the optional `mixer` block of a document (schema.h). Kept out of schema.cpp so the block
// is one self-contained unit.
namespace sf {

namespace {

MixInsert parseInsert(const Rd& r) {
  r.requireObject();
  MixInsert i;
  i.id = r.field(QStringLiteral("id")).str();
  i.type = r.field(QStringLiteral("type")).str();
  i.bypass = r.field(QStringLiteral("bypass")).boolOr(false);
  const Rd params = r.field(QStringLiteral("params"));
  if (!params.missing()) {
    params.requireObject();
    i.params = parseEffectParams(params);
  }
  const Rd auto_ = r.field(QStringLiteral("automation"));
  if (!auto_.missing()) {
    auto_.requireObject();
    i.automation = parseKeyframeMap(auto_);
  }
  i.sidechain = r.field(QStringLiteral("sidechain")).optStr();
  i.state = r.field(QStringLiteral("state")).optStr();
  return i;
}

MixSend parseSend(const Rd& r) {
  r.requireObject();
  MixSend s;
  s.target = r.field(QStringLiteral("target")).str();
  s.level = r.field(QStringLiteral("level")).numOr(1.0, {.min = 0.0});
  s.preFader = r.field(QStringLiteral("preFader")).boolOr(false);
  return s;
}

MixNode parseNode(const Rd& r) {
  r.requireObject();
  MixNode n;
  n.id = r.field(QStringLiteral("id")).str();
  n.name = r.field(QStringLiteral("name")).optStr();
  n.volume = r.field(QStringLiteral("volume")).numOr(1.0, {.min = 0.0});
  n.pan = r.field(QStringLiteral("pan")).numOr(0.0, {.min = -1.0, .max = 1.0});
  n.muted = r.field(QStringLiteral("muted")).boolOr(false);
  n.solo = r.field(QStringLiteral("solo")).boolOr(false);
  n.output = r.field(QStringLiteral("output")).optStr();
  n.inserts = r.field(QStringLiteral("inserts")).listOr(parseInsert);
  n.sends = r.field(QStringLiteral("sends")).listOr(parseSend);
  const Rd a = r.field(QStringLiteral("automation"));
  if (!a.missing()) {
    a.requireObject();
    n.automation = parseKeyframeMap(a);
  }
  return n;
}

QJsonObject insertJson(const MixInsert& i) {
  QJsonObject o{{QStringLiteral("id"), i.id}, {QStringLiteral("type"), i.type}, {QStringLiteral("bypass"), i.bypass},
                {QStringLiteral("params"), toJson(i.params)}};
  if (!i.automation.empty()) o.insert(QStringLiteral("automation"), toJson(i.automation));
  if (i.sidechain) o.insert(QStringLiteral("sidechain"), *i.sidechain);
  if (i.state) o.insert(QStringLiteral("state"), *i.state);
  return o;
}

QJsonObject sendJson(const MixSend& s) {
  return {{QStringLiteral("target"), s.target}, {QStringLiteral("level"), s.level}, {QStringLiteral("preFader"), s.preFader}};
}

QJsonObject nodeJson(const MixNode& n) {
  QJsonObject o{{QStringLiteral("id"), n.id},       {QStringLiteral("volume"), n.volume}, {QStringLiteral("pan"), n.pan},
                {QStringLiteral("muted"), n.muted}, {QStringLiteral("solo"), n.solo}};
  if (n.name) o.insert(QStringLiteral("name"), *n.name);
  if (n.output) o.insert(QStringLiteral("output"), *n.output);
  QJsonArray inserts, sends;
  for (const MixInsert& i : n.inserts) inserts.append(insertJson(i));
  for (const MixSend& s : n.sends) sends.append(sendJson(s));
  o.insert(QStringLiteral("inserts"), inserts);
  o.insert(QStringLiteral("sends"), sends);
  if (!n.automation.empty()) o.insert(QStringLiteral("automation"), toJson(n.automation));
  return o;
}

} // namespace

Mixer parseMixer(const Rd& r) {
  r.requireObject();
  Mixer m;
  m.strips = r.field(QStringLiteral("strips")).listOr(parseNode);
  m.buses = r.field(QStringLiteral("buses")).listOr(parseNode);
  const Rd master = r.field(QStringLiteral("master"));
  if (!master.missing()) m.master = parseNode(master);
  m.master.id = QStringLiteral("master");
  return m;
}

QJsonObject toJson(const Mixer& m) {
  QJsonArray strips, buses;
  for (const MixNode& n : m.strips) strips.append(nodeJson(n));
  for (const MixNode& n : m.buses) buses.append(nodeJson(n));
  return {{QStringLiteral("strips"), strips}, {QStringLiteral("buses"), buses}, {QStringLiteral("master"), nodeJson(m.master)}};
}

} // namespace sf
