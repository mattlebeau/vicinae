#include "services/ai/wyoming/wyoming-protocol.hpp"
#include <algorithm>
#include <limits>
#include <QJsonArray>
#include <QJsonDocument>

namespace AI::wyoming {

namespace {
constexpr auto VERSION = "1.5.4";

QJsonObject audioFormat(int rate) { return QJsonObject{{"rate", rate}, {"width", 2}, {"channels", 1}}; }

std::string str(const QJsonValue &value) { return value.toString().toStdString(); }
} // namespace

QByteArray encode(const QString &type, const QJsonObject &data, const QByteArray &payload) {
  QJsonObject header{{"type", type}, {"version", VERSION}};
  QByteArray body;
  if (!data.isEmpty()) {
    body = QJsonDocument(data).toJson(QJsonDocument::Compact);
    header["data_length"] = static_cast<qint64>(body.size());
  }
  if (!payload.isEmpty()) header["payload_length"] = static_cast<qint64>(payload.size());

  QByteArray out = QJsonDocument(header).toJson(QJsonDocument::Compact);
  out.reserve(out.size() + 1 + body.size() + payload.size());
  out.append('\n').append(body).append(payload);
  return out;
}

std::vector<Event> EventParser::feed(const QByteArray &bytes) {
  std::vector<Event> events;
  if (m_error) return events;
  m_buffer.append(bytes);

  while (true) {
    if (!m_pending) {
      const auto newline = m_buffer.indexOf('\n');
      if (newline < 0) {
        if (m_buffer.size() > MAX_HEADER_BYTES) m_error = QStringLiteral("Event header too long");
        break;
      }
      const auto line = m_buffer.left(newline).trimmed();
      m_buffer.remove(0, newline + 1);
      if (line.isEmpty()) continue;

      QJsonParseError parseError;
      const auto doc = QJsonDocument::fromJson(line, &parseError);
      if (!doc.isObject() || !doc.object().value("type").isString()) {
        m_error = QStringLiteral("Malformed event header: %1").arg(parseError.errorString());
        break;
      }
      const auto header = doc.object();
      m_pending = Event{.type = header.value("type").toString(), .data = header.value("data").toObject()};
      m_dataLength = header.value("data_length").toInteger(0);
      m_payloadLength = header.value("payload_length").toInteger(0);
      if (m_dataLength < 0 || m_payloadLength < 0) {
        m_error = QStringLiteral("Negative event length");
        break;
      }
    }

    if (m_buffer.size() < m_dataLength + m_payloadLength) break;

    if (m_dataLength > 0) {
      const auto extra = QJsonDocument::fromJson(m_buffer.left(m_dataLength));
      if (!extra.isObject()) {
        m_error = QStringLiteral("Malformed event data");
        break;
      }
      // data from the body takes precedence over inline data, as in the reference implementation
      const auto obj = extra.object();
      for (auto it = obj.begin(); it != obj.end(); ++it) {
        m_pending->data.insert(it.key(), it.value());
      }
    }
    m_pending->payload = m_buffer.mid(m_dataLength, m_payloadLength);
    m_buffer.remove(0, m_dataLength + m_payloadLength);
    events.push_back(std::move(*m_pending));
    m_pending.reset();
  }

  return events;
}

std::vector<AsrModel> parseAsrModels(const QJsonObject &info) {
  std::vector<AsrModel> models;
  for (const auto &programValue : info.value("asr").toArray()) {
    const auto program = programValue.toObject();
    if (!program.value("installed").toBool(true)) continue;

    for (const auto &modelValue : program.value("models").toArray()) {
      const auto model = modelValue.toObject();
      if (!model.value("installed").toBool(true)) continue;

      AsrModel entry{
          .program = str(program.value("name")),
          .name = str(model.value("name")),
          .description = str(model.value("description")),
      };
      for (const auto &lang : model.value("languages").toArray()) {
        entry.languages.push_back(str(lang));
      }
      if (!entry.name.empty()) models.push_back(std::move(entry));
    }
  }
  return models;
}

QByteArray transcriptionRequest(std::span<const std::int16_t> samples, int rate,
                                const std::optional<std::string> &model,
                                const std::optional<std::string> &language, int chunkMs) {
  QJsonObject transcribe;
  if (model) transcribe["name"] = QString::fromStdString(*model);
  if (language && !language->empty()) transcribe["language"] = QString::fromStdString(*language);

  const auto format = audioFormat(rate);
  const std::size_t chunkSamples = std::max<std::size_t>(1, static_cast<std::size_t>(rate) * chunkMs / 1000);

  QByteArray out = encode("transcribe", transcribe);
  out.append(encode("audio-start", format));
  for (std::size_t offset = 0; offset < samples.size(); offset += chunkSamples) {
    const auto chunk = samples.subspan(offset, std::min(chunkSamples, samples.size() - offset));
    // PCM16 little-endian; x86 and ARM hosts are little-endian already
    QByteArray pcm(reinterpret_cast<const char *>(chunk.data()),
                   static_cast<qsizetype>(chunk.size() * sizeof(std::int16_t)));
    out.append(encode("audio-chunk", format, pcm));
  }
  out.append(encode("audio-stop"));
  return out;
}

std::vector<std::size_t> splitPoints(std::span<const float> samples, int rate, std::size_t maxSamples) {
  std::vector<std::size_t> ends;
  const std::size_t window = std::max<std::size_t>(1, static_cast<std::size_t>(rate) / 10);
  maxSamples = std::max(maxSamples, window * 2);

  std::size_t start = 0;
  while (samples.size() - start > maxSamples) {
    const std::size_t limit = start + maxSamples;
    std::size_t bestCut = limit;
    double bestEnergy = std::numeric_limits<double>::max();

    // windows that end by `limit`, from the middle of the span on
    for (std::size_t w = start + maxSamples / 2; w + window <= limit; w += window / 2) {
      double energy = 0;
      for (std::size_t i = w; i < w + window; ++i) {
        energy += static_cast<double>(samples[i]) * samples[i];
      }
      // ties go to the later window, keeping pieces long
      if (energy <= bestEnergy) {
        bestEnergy = energy;
        bestCut = w + window / 2;
      }
    }
    ends.push_back(bestCut);
    start = bestCut;
  }
  ends.push_back(samples.size());
  return ends;
}

} // namespace AI::wyoming
