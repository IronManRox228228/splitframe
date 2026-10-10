#include "agent/router.h"

#include <algorithm>
#include <set>

namespace sf::agent {

namespace {

const auto ci = QRegularExpression::CaseInsensitiveOption;

const QRegularExpression kStrongAssemble(QStringLiteral(R"(\b(teaser|trailer|highlights?|first cut|rough cut|recap|sizzle|summary)\b)"), ci);
const QRegularExpression kCutting(QStringLiteral(R"(\b(trim|shorten|lose|chop|cut|drop|remove|delete|take (it|that|this) (off|out)|get rid)\b)"), ci);
const QRegularExpression kQuestionStart(QStringLiteral(R"(^(what|how|why|when|where|which|who|can you|could you explain|is |are |does |do you|tell me)\b)"), ci);

} // namespace

Router::Router() {
  rules_ = {
    {QStringLiteral("undo"), QRegularExpression(QStringLiteral(R"(\b(undo|revert|roll ?back|put (it|that|them) back|go back)\b)"), ci)},
    {QStringLiteral("redo"), QRegularExpression(QStringLiteral(R"(\bredo\b)"), ci)},
    {QStringLiteral("export"), QRegularExpression(QStringLiteral(R"(\b(export|render (it|this|the video|out)|save (it |this )?as (an? )?(mp4|video|file))\b)"), ci)},
    {QStringLiteral("canvas"), QRegularExpression(QStringLiteral(R"(\b(vertical|portrait|horizontal|landscape|square|(9|16|4|1|21):(16|9|5|3|1)|reels?|tiktok|shorts|aspect( ratio)?|fps|frame ?rate|canvas)\b)"), ci)},
    {QStringLiteral("captions"), QRegularExpression(QStringLiteral(R"(\b(captions?|subtitles?)\b)"), ci)},
    {QStringLiteral("title"), QRegularExpression(QStringLiteral(R"(\b(titles?|heading|lower third|text (that )?(says|reading))\b)"), ci)},
    {QStringLiteral("pauses"), QRegularExpression(QStringLiteral(R"(\b(pauses?|silen(ce|ces|t)|dead air)\b)"), ci)},
    {QStringLiteral("fillers"), QRegularExpression(QStringLiteral(R"(\b(fillers?|ums?|umm+|uhs?|uhh+|ahs?|ahh+|erm+|hmm+|you know)\b)"), ci)},
    {QStringLiteral("retakes"), QRegularExpression(QStringLiteral(R"(\b(re-?takes?|repeated takes?|false starts?|said (it|that)? ?again|(bad|first|botched|failed) (take|attempt)|flubs?|botch(ed)?|fumbl(e|ed)|stumbl(e|ed))\b)"), ci)},
    {QStringLiteral("music"), QRegularExpression(QStringLiteral(R"(\b(music|soundtrack|background (track|audio)|duck(ing)?)\b)"), ci)},
    {QStringLiteral("beats"), QRegularExpression(QStringLiteral(R"(\b(beats?|rhythm|bpm)\b)"), ci)},
    {QStringLiteral("audio"), QRegularExpression(QStringLiteral(R"(\b(audio|volume|loudness|lufs|mixer|pan|mute|solo|compress(or|ion)?|eq|limiter|clap|plugin|sound)\b)"), ci)},
    {QStringLiteral("color"), QRegularExpression(QStringLiteral(R"(\b(colou?rs?|grades?|grading|luts?|exposure|saturation|lift|gamma|gain|contrast|white balance|ocio|aces|shot match(ing)?)\b)"), ci)},
    {QStringLiteral("assemble"), QRegularExpression(QStringLiteral(R"(\b(teaser|trailer|highlights?|first cut|rough cut|recap|sizzle|summary|\d+[- ]?(s|sec|secs|seconds?|min|minutes?)\b.*\b(from|of))\b)"), ci)},
    {QStringLiteral("clips"), QRegularExpression(QStringLiteral(R"(\b(trim|cut|split|shorten|move|reorder|swap|delete|remove|duplicate|speed|slow|faster|first|last|before|after|play|plays)\b)"), ci)}
  };
}

bool Router::isQuestion(const QString& message) {
  const QString trimmed = message.trimmed();
  return trimmed.endsWith(QLatin1Char('?')) && kQuestionStart.match(trimmed).hasMatch();
}

RouteResult Router::route(const QString& message, const QString& recentContext) const {
  RouteResult res;

  if (isQuestion(message)) {
    res.isQuestion = true;
    res.via = QStringLiteral("question");
    res.primary = SpecialistKind::Editor;
    return res;
  }

  // 1. Match rules
  QStringList foundIntents;
  for (const auto& r : rules_) {
    if (r.regex.match(message).hasMatch()) {
      foundIntents.append(r.intent);
    }
  }

  // Follow-up context check (e.g. "make them bigger" after captions)
  static const QRegularExpression kStyle(QStringLiteral(R"(\b(bigger|larger|smaller|font|size|colou?r|higher|lower|top|bottom|bolder)\b)"), ci);
  if (foundIntents.isEmpty() && kStyle.match(message).hasMatch() && recentContext.contains(QStringLiteral("caption"), Qt::CaseInsensitive)) {
    foundIntents.append(QStringLiteral("captions"));
  }

  // Disambiguate "cut the first 2 seconds" vs assemble
  if (foundIntents.contains(QStringLiteral("assemble")) && !kStrongAssemble.match(message).hasMatch() && kCutting.match(message).hasMatch()) {
    foundIntents.removeAll(QStringLiteral("assemble"));
    if (!foundIntents.contains(QStringLiteral("clips"))) {
      foundIntents.append(QStringLiteral("clips"));
    }
  }

  res.intents = foundIntents;

  // Determine domain participation
  bool hasEditor = false;
  bool hasColour = false;
  bool hasAudio = false;

  for (const QString& i : foundIntents) {
    if (i == QStringLiteral("color")) {
      hasColour = true;
    } else if (i == QStringLiteral("music") || i == QStringLiteral("beats") || i == QStringLiteral("audio")) {
      hasAudio = true;
    } else {
      hasEditor = true;
    }
  }

  // If nothing matched, default to Editor
  if (!hasEditor && !hasColour && !hasAudio) {
    hasEditor = true;
  }

  // Assign primary and secondary
  if (hasColour && !hasEditor && !hasAudio) {
    res.primary = SpecialistKind::Colourist;
  } else if (hasAudio && !hasEditor && !hasColour) {
    res.primary = SpecialistKind::Audio;
  } else if (hasEditor) {
    res.primary = SpecialistKind::Editor;
    if (hasAudio) res.secondary.push_back(SpecialistKind::Audio);
    if (hasColour) res.secondary.push_back(SpecialistKind::Colourist);
  } else if (hasAudio) {
    res.primary = SpecialistKind::Audio;
    if (hasColour) res.secondary.push_back(SpecialistKind::Colourist);
  }

  // Check if planning is needed:
  // - assemble is a high-level creative job
  // - cross-domain cooperation needs an orchestrating plan
  // - 2 or more distinct jobs (e.g. pauses + music)
  int distinctJobs = 0;
  std::set<QString> jobSet;
  for (const QString& i : foundIntents) {
    if (i == QStringLiteral("fillers") || i == QStringLiteral("retakes")) {
      jobSet.insert(QStringLiteral("pauses"));
    } else if (i != QStringLiteral("clips") && i != QStringLiteral("undo") && i != QStringLiteral("redo")) {
      jobSet.insert(i);
    }
  }
  distinctJobs = static_cast<int>(jobSet.size());

  if (foundIntents.contains(QStringLiteral("assemble")) || !res.secondary.empty() || distinctJobs >= 2) {
    res.needsPlan = true;
  }

  return res;
}

} // namespace sf::agent
