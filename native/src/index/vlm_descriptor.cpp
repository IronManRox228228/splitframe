#include "index/vlm_descriptor.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QUuid>

namespace sf::index {

VlmDescriptor::VlmDescriptor(std::shared_ptr<agent::LlmClient> llm)
  : llm_(std::move(llm)) {}

VlmDescriptor::~VlmDescriptor() = default;

void VlmDescriptor::setLlmClient(std::shared_ptr<agent::LlmClient> llm) {
  llm_ = std::move(llm);
}

VlmTag VlmDescriptor::describeKeyframe(const QImage& keyframe,
                                      const QString& assetId,
                                      const QString& sceneId) {
  VlmTag tag;
  tag.id = QStringLiteral("vlm_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
  tag.assetId = assetId;
  tag.sceneId = sceneId;

  if (llm_ && !keyframe.isNull()) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    // Downscale for vision VLM throughput (512px width is plenty for scene understanding)
    keyframe.scaledToWidth(512, Qt::SmoothTransformation).save(&buf, "JPG", 80);
    const QString b64 = QString::fromLatin1(bytes.toBase64());

    const QString systemPrompt = QStringLiteral(
      "You are a computer vision assistant for a professional video editor.\n"
      "Describe the given image in exactly one concise sentence.\n"
      "Identify the shot type (wide, medium, close-up) and primary subject.\n"
      "Output valid JSON: {\"summary\": \"...\", \"shot_type\": \"...\", \"subject\": \"...\"}"
    );

    agent::ChatMessage msg;
    msg.role = QStringLiteral("user");
    // multimodal content array: a plain-text data URL is never seen by vision models
    msg.contentParts = QJsonArray{
      QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                  {QStringLiteral("text"), QStringLiteral("Describe this keyframe.")}},
      QJsonObject{{QStringLiteral("type"), QStringLiteral("image_url")},
                  {QStringLiteral("image_url"), QJsonObject{{QStringLiteral("url"),
                      QStringLiteral("data:image/jpeg;base64,%1").arg(b64)}}}}};
    msg.content = QStringLiteral("[keyframe attached]");

    QJsonObject resp = llm_->complete(systemPrompt, {msg});
    QString content = resp.value(QStringLiteral("content")).toString().trimmed();

    // Strip markdown code fences if model returned ```json ... ```
    if (content.startsWith(QLatin1String("```"))) {
      int firstNl = content.indexOf(QLatin1Char('\n'));
      int lastFence = content.lastIndexOf(QLatin1String("```"));
      if (firstNl >= 0 && lastFence > firstNl) {
        content = content.mid(firstNl + 1, lastFence - firstNl - 1).trimmed();
      }
    }

    QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
    if (doc.isObject()) {
      QJsonObject obj = doc.object();
      tag.summary = obj.value(QStringLiteral("summary")).toString();
      tag.shotType = obj.value(QStringLiteral("shot_type")).toString();
      tag.subject = obj.value(QStringLiteral("subject")).toString();
    } else if (!content.isEmpty()) {
      // A free-form reply is still a real description; shot/subject stay unset (not invented).
      tag.summary = content;
    }
  }

  // No fabricated default here: when no VLM was available (or it failed) a tag that pretends
  // to describe the frame must not enter the catalog/FTS. The caller skips empty summaries.
  return tag;
}

} // namespace sf::index
