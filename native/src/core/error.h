#pragma once

#include <QString>
#include <stdexcept>
#include <string>

namespace sf {

// Thrown when JSON does not match the schema. what() reads "items[2].durationFrames: ...", the
// same path-first shape as a zod issue, so messages can be shown to users and agents verbatim.
class SchemaError : public std::runtime_error {
 public:
  SchemaError(QString path, QString issue)
      : std::runtime_error(format(path, issue)), path_(std::move(path)), issue_(std::move(issue)) {}

  const QString& path() const { return path_; }
  const QString& issue() const { return issue_; }

 private:
  static std::string format(const QString& path, const QString& issue) {
    return (path.isEmpty() ? issue : path + QStringLiteral(": ") + issue).toStdString();
  }
  QString path_;
  QString issue_;
};

// A well-formed op that cannot apply to the current document. `hint` tells an agent or the UI how
// to recover, same as OpError in packages/editor-core.
class OpError : public std::runtime_error {
 public:
  explicit OpError(const QString& message,
                   const QString& hint = QStringLiteral("Check the referenced ids exist and the values are valid."))
      : std::runtime_error(message.toStdString()), hint_(hint) {}

  const QString& hint() const { return hint_; }

 private:
  QString hint_;
};

} // namespace sf
