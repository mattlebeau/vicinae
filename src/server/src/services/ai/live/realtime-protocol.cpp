#include "services/ai/live/realtime-protocol.hpp"
#include <algorithm>
#include <cmath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include "services/ai/live/chunk-planner.hpp"
#include "services/ai/live/live-transcriber.hpp"

namespace AI::realtime {

namespace {
constexpr int SILENCE_DURATION_MS = 500;
constexpr double VAD_THRESHOLD = 0.5;
constexpr int PREFIX_PADDING_MS = 300;

QString errorMessage(const QJsonObject &event) {
  const auto error = event.value(QStringLiteral("error"));
  if (error.isObject()) {
    const auto message = error.toObject().value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) return message;
  }
  if (error.isString()) return error.toString();
  return QStringLiteral("unknown error");
}
} // namespace

QUrl endpoint(const QString &httpBase, Dialect dialect, const std::string &model, const QString &pathOverride) {
  QUrl url(httpBase);
  url.setScheme(url.scheme() == QStringLiteral("https") ? QStringLiteral("wss") : QStringLiteral("ws"));

  if (pathOverride.startsWith(QLatin1Char('/'))) {
    url.setPath(pathOverride);
  } else {
    auto path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
      path.chop(1);
    }
    url.setPath(path + QLatin1Char('/') + (pathOverride.isEmpty() ? QStringLiteral("realtime") : pathOverride));
  }

  QUrlQuery query;
  if (dialect == Dialect::Ga) {
    query.addQueryItem(QStringLiteral("intent"), QStringLiteral("transcription"));
  } else {
    query.addQueryItem(QStringLiteral("model"), QString::fromStdString(model));
  }
  url.setQuery(query);
  return url;
}

QByteArray sessionUpdate(Dialect dialect, const std::string &model, const std::optional<std::string> &language) {
  QJsonObject transcription{{QStringLiteral("model"), QString::fromStdString(model)}};
  if (language) transcription.insert(QStringLiteral("language"), QString::fromStdString(*language));

  QJsonObject turnDetection{
      {QStringLiteral("type"), QStringLiteral("server_vad")},
      {QStringLiteral("silence_duration_ms"), SILENCE_DURATION_MS},
  };

  QJsonObject session;
  if (dialect == Dialect::Ga) {
    session = QJsonObject{
        {QStringLiteral("type"), QStringLiteral("transcription")},
        {QStringLiteral("audio"),
         QJsonObject{{QStringLiteral("input"),
                      QJsonObject{
                          {QStringLiteral("format"),
                           QJsonObject{{QStringLiteral("type"), QStringLiteral("audio/pcm")},
                                       {QStringLiteral("rate"), SAMPLE_RATE}}},
                          {QStringLiteral("transcription"), transcription},
                          {QStringLiteral("turn_detection"), turnDetection},
                      }}}},
    };
  } else {
    // Speaches drops a turn_detection that lacks any of these
    turnDetection.insert(QStringLiteral("threshold"), VAD_THRESHOLD);
    turnDetection.insert(QStringLiteral("prefix_padding_ms"), PREFIX_PADDING_MS);
    // without this the server answers every segment with a chat response
    turnDetection.insert(QStringLiteral("create_response"), false);
    session = QJsonObject{
        {QStringLiteral("input_audio_transcription"), transcription},
        {QStringLiteral("turn_detection"), turnDetection},
    };
  }

  const QJsonObject event{{QStringLiteral("type"), QStringLiteral("session.update")},
                          {QStringLiteral("session"), session}};
  return QJsonDocument(event).toJson(QJsonDocument::Compact);
}

QByteArray appendAudio(std::span<const float> samples) {
  QByteArray pcm;
  pcm.resize(static_cast<qsizetype>(samples.size() * sizeof(std::int16_t)));
  auto *out = reinterpret_cast<std::int16_t *>(pcm.data());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const auto s = std::clamp(samples[i], -1.0f, 1.0f);
    // little-endian on every platform we build for
    out[i] = static_cast<std::int16_t>(std::lround(s * 32767.0f));
  }

  const QJsonObject event{{QStringLiteral("type"), QStringLiteral("input_audio_buffer.append")},
                          {QStringLiteral("audio"), QString::fromLatin1(pcm.toBase64())}};
  return QJsonDocument(event).toJson(QJsonDocument::Compact);
}

TranscriptAssembler::Item &TranscriptAssembler::item(const QString &id) {
  const auto key = id.toStdString();
  auto it = m_items.find(key);
  if (it == m_items.end()) {
    m_order.emplace_back(id);
    it = m_items.emplace(key, Item{}).first;
  }
  return it->second;
}

TranscriptAssembler::Event TranscriptAssembler::handle(const QByteArray &message) {
  const auto doc = QJsonDocument::fromJson(message);
  if (!doc.isObject()) return {};
  const auto event = doc.object();
  const auto type = event.value(QStringLiteral("type")).toString();
  const auto itemId = event.value(QStringLiteral("item_id")).toString();

  if (type == QStringLiteral("session.created") || type == QStringLiteral("transcription_session.created")) {
    return {.kind = EventKind::SessionCreated};
  }
  if (type == QStringLiteral("error")) return {.kind = EventKind::Error, .message = errorMessage(event)};

  if (itemId.isEmpty()) return {};

  if (type == QStringLiteral("input_audio_buffer.committed") ||
      type == QStringLiteral("input_audio_buffer.speech_started")) {
    // reserves the item's place, so segments that finish out of order still read in order
    item(itemId);
    return {};
  }
  if (type == QStringLiteral("conversation.item.input_audio_transcription.delta")) {
    auto &entry = item(itemId);
    if (entry.done) return {};
    entry.text += event.value(QStringLiteral("delta")).toString();
    return {.kind = EventKind::TextChanged};
  }
  if (type == QStringLiteral("conversation.item.input_audio_transcription.completed")) {
    auto &entry = item(itemId);
    entry.text = QString::fromStdString(
        live::cleanTranscript(event.value(QStringLiteral("transcript")).toString().toStdString()));
    entry.done = true;
    return {.kind = EventKind::TextChanged};
  }
  if (type == QStringLiteral("conversation.item.input_audio_transcription.failed")) {
    auto &entry = item(itemId);
    entry.done = true;
    return {.kind = EventKind::TextChanged, .message = errorMessage(event)};
  }
  return {};
}

QString TranscriptAssembler::committed() const {
  QString text;
  for (const auto &id : m_order) {
    const auto &entry = m_items.at(id.toStdString());
    if (!entry.done) break;
    text = joinTranscript(text, entry.text);
  }
  return text;
}

QString TranscriptAssembler::tentative() const {
  QString text;
  bool pending = false;
  for (const auto &id : m_order) {
    const auto &entry = m_items.at(id.toStdString());
    pending = pending || !entry.done;
    if (pending) text = joinTranscript(text, entry.text);
  }
  return text;
}

} // namespace AI::realtime
