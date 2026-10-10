#pragma once

// Thin stateless router: classifies user requests into intents, determines
// primary and secondary specialists, and decides whether a plan is required.

#include "agent/types.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <vector>

namespace sf::agent {

struct RouteResult {
  SpecialistKind primary = SpecialistKind::Editor;
  std::vector<SpecialistKind> secondary;
  QStringList intents;
  bool needsPlan = false;
  bool isQuestion = false;
  QString via = QStringLiteral("rules");

  QJsonObject toJson() const {
    QJsonArray secArr;
    for (SpecialistKind s : secondary) secArr.append(specialistName(s));

    QJsonArray intArr;
    for (const QString& i : intents) intArr.append(i);

    return QJsonObject{
      {QStringLiteral("primary"), specialistName(primary)},
      {QStringLiteral("secondary"), secArr},
      {QStringLiteral("intents"), intArr},
      {QStringLiteral("needsPlan"), needsPlan},
      {QStringLiteral("isQuestion"), isQuestion},
      {QStringLiteral("via"), via}
    };
  }
};

class Router {
public:
  Router();
  ~Router() = default;

  // Route user message to intents and specialists
  RouteResult route(const QString& message, const QString& recentContext = {}) const;

  // Check if text is a non-mutating inquiry
  static bool isQuestion(const QString& message);

private:
  struct Rule {
    QString intent;
    QRegularExpression regex;
  };
  std::vector<Rule> rules_;
};

} // namespace sf::agent
