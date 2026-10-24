#include "agent/peer_bus.h"
#include "agent/trace.h"

#include <QUuid>
#include <algorithm>

namespace sf::agent {

PeerBus::PeerBus(QObject* parent) : QObject(parent) {}

PeerResponse PeerBus::send(PeerRequest req) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  if (req.id.isEmpty()) {
    req.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  }

  const QString runId = !currentRunId_.isEmpty() ? currentRunId_ : req.id;

  // 1. Guard against self-requests
  if (req.from == req.to) {
    PeerResponse res{
      .requestId = req.id,
      .status = QStringLiteral("declined"),
      .ops = {},
      .outputs = {},
      .note = QStringLiteral("self-request not allowed")
    };
    log_.push_back(PeerLogEntry{
      .timestampMs = QDateTime::currentMSecsSinceEpoch(),
      .requestId = req.id,
      .from = req.from,
      .to = req.to,
      .kind = req.kind,
      .reason = req.reason,
      .args = req.args,
      .status = res.status,
      .note = res.note,
      .outputs = res.outputs
    });
    emit messageLogged(log_.back());

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = specialistName(req.from),
      .role = QStringLiteral("peer"),
      .action = QStringLiteral("error"),
      .inputSummary = QStringLiteral("%1: %2").arg(req.kind, req.reason),
      .outputSummary = res.note,
      .targetAgent = specialistName(req.to),
      .tool = req.kind,
      .status = QStringLiteral("error"),
      .retryCount = req.depth,
      .error = res.note
    }, projectDir_);

    return res;
  }

  // 2. Guard against chain depth explosion (>= 3 hops)
  if (req.depth >= kMaxChainDepth) {
    PeerResponse res{
      .requestId = req.id,
      .status = QStringLiteral("declined"),
      .ops = {},
      .outputs = {},
      .note = QStringLiteral("max chain depth (%1) exceeded").arg(kMaxChainDepth)
    };
    log_.push_back(PeerLogEntry{
      .timestampMs = QDateTime::currentMSecsSinceEpoch(),
      .requestId = req.id,
      .from = req.from,
      .to = req.to,
      .kind = req.kind,
      .reason = req.reason,
      .args = req.args,
      .status = res.status,
      .note = res.note,
      .outputs = res.outputs
    });
    emit messageLogged(log_.back());

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = specialistName(req.from),
      .role = QStringLiteral("peer"),
      .action = QStringLiteral("escalate"),
      .inputSummary = QStringLiteral("%1: %2").arg(req.kind, req.reason),
      .outputSummary = res.note,
      .targetAgent = specialistName(req.to),
      .tool = req.kind,
      .status = QStringLiteral("error"),
      .retryCount = req.depth,
      .error = res.note
    }, projectDir_);

    return res;
  }

  // 3. Cycle detection: if recipient is already in current active call chain
  if (std::find(activeStack_.begin(), activeStack_.end(), req.to) != activeStack_.end()) {
    PeerResponse res{
      .requestId = req.id,
      .status = QStringLiteral("declined"),
      .ops = {},
      .outputs = {},
      .note = QStringLiteral("cycle detected: %1 already in call stack").arg(specialistName(req.to))
    };
    log_.push_back(PeerLogEntry{
      .timestampMs = QDateTime::currentMSecsSinceEpoch(),
      .requestId = req.id,
      .from = req.from,
      .to = req.to,
      .kind = req.kind,
      .reason = req.reason,
      .args = req.args,
      .status = res.status,
      .note = res.note,
      .outputs = res.outputs
    });
    emit messageLogged(log_.back());

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = specialistName(req.from),
      .role = QStringLiteral("peer"),
      .action = QStringLiteral("error"),
      .inputSummary = QStringLiteral("%1: %2").arg(req.kind, req.reason),
      .outputSummary = res.note,
      .targetAgent = specialistName(req.to),
      .tool = req.kind,
      .status = QStringLiteral("error"),
      .retryCount = req.depth,
      .error = res.note
    }, projectDir_);

    return res;
  }

  // 4. Find handler
  PeerHandler handler = nullptr;
  auto it = handlers_.find({req.to, req.kind});
  if (it != handlers_.end()) {
    handler = it->second;
  } else {
    auto defIt = defaultHandlers_.find(req.to);
    if (defIt != defaultHandlers_.end()) {
      handler = defIt->second;
    }
  }

  if (!handler) {
    PeerResponse res{
      .requestId = req.id,
      .status = QStringLiteral("declined"),
      .ops = {},
      .outputs = {},
      .note = QStringLiteral("no handler registered on %1 for kind '%2'").arg(specialistName(req.to), req.kind)
    };
    log_.push_back(PeerLogEntry{
      .timestampMs = QDateTime::currentMSecsSinceEpoch(),
      .requestId = req.id,
      .from = req.from,
      .to = req.to,
      .kind = req.kind,
      .reason = req.reason,
      .args = req.args,
      .status = res.status,
      .note = res.note,
      .outputs = res.outputs
    });
    emit messageLogged(log_.back());

    logAgentTrace(AgentTraceRecord{
      .runId = runId,
      .agentName = specialistName(req.from),
      .role = QStringLiteral("peer"),
      .action = QStringLiteral("error"),
      .inputSummary = QStringLiteral("%1: %2").arg(req.kind, req.reason),
      .outputSummary = res.note,
      .targetAgent = specialistName(req.to),
      .tool = req.kind,
      .status = QStringLiteral("error"),
      .retryCount = req.depth,
      .error = res.note
    }, projectDir_);

    return res;
  }

  // 5. Invoke handler under active stack tracking (originator + recipient)
  activeStack_.push_back(req.from);
  activeStack_.push_back(req.to);
  PeerResponse res;
  try {
    // propagate depth
    PeerRequest childReq = req;
    childReq.depth = req.depth + 1;
    res = handler(childReq);
    if (res.requestId.isEmpty()) res.requestId = req.id;
  } catch (...) {
    res.requestId = req.id;
    res.status = QStringLiteral("declined");
    res.note = QStringLiteral("handler threw exception");
  }
  activeStack_.pop_back();
  activeStack_.pop_back();

  // 6. Record in audit trail
  log_.push_back(PeerLogEntry{
    .timestampMs = QDateTime::currentMSecsSinceEpoch(),
    .requestId = req.id,
    .from = req.from,
    .to = req.to,
    .kind = req.kind,
    .reason = req.reason,
    .args = req.args,
    .status = res.status,
    .note = res.note,
    .outputs = res.outputs
  });
  emit messageLogged(log_.back());

  const bool isCritique = (req.kind == QStringLiteral("critique") || req.kind.startsWith(QStringLiteral("critique")) ||
                           req.kind == QStringLiteral("review") || req.kind == QStringLiteral("peer_review") ||
                           req.kind.contains(QStringLiteral("critique")) || req.kind.contains(QStringLiteral("review")));
  const QString action = isCritique ? QStringLiteral("critique") : QStringLiteral("handoff");

  QString traceStatus;
  if (res.status == QStringLiteral("done") || res.status == QStringLiteral("ok")) {
    traceStatus = QStringLiteral("success");
  } else if (res.status == QStringLiteral("needs_revision")) {
    traceStatus = QStringLiteral("needs_revision");
  } else if (res.status == QStringLiteral("declined")) {
    traceStatus = isCritique ? QStringLiteral("needs_revision") : QStringLiteral("error");
  } else {
    traceStatus = QStringLiteral("error");
  }

  logAgentTrace(AgentTraceRecord{
    .runId = runId,
    .agentName = specialistName(req.from),
    .role = QStringLiteral("peer"),
    .action = action,
    .inputSummary = QStringLiteral("%1: %2").arg(req.kind, req.reason),
    .outputSummary = res.note,
    .targetAgent = specialistName(req.to),
    .tool = req.kind,
    .status = traceStatus,
    .retryCount = req.depth,
    .error = (traceStatus == QStringLiteral("error")) ? res.note : QString()
  }, projectDir_);

  return res;
}

void PeerBus::registerHandler(SpecialistKind to, const QString& kind, PeerHandler handler) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  handlers_[{to, kind}] = std::move(handler);
}

void PeerBus::setDefaultHandler(SpecialistKind to, PeerHandler handler) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  defaultHandlers_[to] = std::move(handler);
}

std::vector<PeerLogEntry> PeerBus::teamThread() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return log_;
}

QJsonArray PeerBus::teamThreadJson() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  QJsonArray arr;
  for (const auto& entry : log_) {
    arr.append(entry.toJson());
  }
  return arr;
}

void PeerBus::clearLog() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  log_.clear();
}

} // namespace sf::agent
