#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <QByteArray>
#include <QJsonObject>
#include <QString>

/**
 * The Wyoming protocol (https://github.com/OHF-Voice/wyoming) used by Home Assistant's speech services.
 *
 * Every event is a JSON header line, optionally followed by `data_length` bytes of JSON data and
 * `payload_length` bytes of binary payload:
 *
 *   {"type":"audio-chunk","data_length":42,"payload_length":3200}\n{"rate":16000,...}<pcm bytes>
 *
 * Older peers put the data inline in the header as `"data":{...}`; both forms are accepted.
 * Pure code without sockets, so it can be unit-tested.
 */
namespace AI::wyoming {

struct Event {
  QString type;
  QJsonObject data;
  QByteArray payload;
};

QByteArray encode(const QString &type, const QJsonObject &data = {}, const QByteArray &payload = {});

/**
 * Splits a byte stream into events. Feed it whatever arrives from the socket.
 */
class EventParser {
public:
  // Complete events in `bytes` and in what was buffered before. Sets `error()` on malformed input.
  std::vector<Event> feed(const QByteArray &bytes);
  const std::optional<QString> &error() const { return m_error; }

private:
  static constexpr qsizetype MAX_HEADER_BYTES = qsizetype{64} * 1024;

  QByteArray m_buffer;
  std::optional<Event> m_pending;
  qsizetype m_dataLength = 0;
  qsizetype m_payloadLength = 0;
  std::optional<QString> m_error;
};

struct AsrModel {
  std::string program;
  std::string name;
  std::string description;
  std::vector<std::string> languages;
};

/**
 * Speech-to-text models announced in an `info` event (reply to `describe`).
 */
std::vector<AsrModel> parseAsrModels(const QJsonObject &info);

/**
 * The audio of one request: `transcribe`, `audio-start`, `audio-chunk`s and `audio-stop`.
 * `samples` is mono 16-bit PCM at `rate`.
 */
QByteArray transcriptionRequest(std::span<const std::int16_t> samples, int rate,
                                const std::optional<std::string> &model,
                                const std::optional<std::string> &language, int chunkMs = 100);

/**
 * Where to cut a recording so that no piece is longer than `maxSamples`. Cuts fall in the quietest
 * 100 ms window of the second half of each allowed span, to avoid splitting a word.
 * Returns the end offsets of the pieces; the last one is always `samples.size()`.
 */
std::vector<std::size_t> splitPoints(std::span<const float> samples, int rate, std::size_t maxSamples);

} // namespace AI::wyoming
