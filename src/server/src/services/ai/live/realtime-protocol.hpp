#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <QByteArray>
#include <QString>
#include <QUrl>

namespace AI::realtime {

/**
 * Flavours of the OpenAI Realtime API used for transcription. Beta is the original shape, which
 * self-hosted servers such as Speaches implement. Ga is the current OpenAI API.
 */
enum class Dialect : std::uint8_t { Beta, Ga };

// both dialects take 24 kHz mono PCM16
constexpr int SAMPLE_RATE = 24000;

/**
 * WebSocket URL for an HTTP API base such as `http://host:8000/v1`. A path override replaces the
 * `/realtime` suffix, e.g. for servers that mount it elsewhere.
 */
QUrl endpoint(const QString &httpBase, Dialect dialect, const std::string &model,
              const QString &pathOverride = {});

QByteArray sessionUpdate(Dialect dialect, const std::string &model, const std::optional<std::string> &language);
QByteArray appendAudio(std::span<const float> samples);

/**
 * Turns server events into the transcript so far. Each speech segment the server detects is an item:
 * items with a final transcript in front of the first unfinished one are committed, the rest is tentative.
 */
class TranscriptAssembler {
public:
  enum class EventKind : std::uint8_t { Ignored, SessionCreated, TextChanged, Error };

  struct Event {
    EventKind kind = EventKind::Ignored;
    QString message;
  };

  Event handle(const QByteArray &message);

  QString committed() const;
  QString tentative() const;

private:
  struct Item {
    QString text;
    bool done = false;
  };

  Item &item(const QString &id);

  std::vector<QString> m_order;
  std::unordered_map<std::string, Item> m_items;
};

} // namespace AI::realtime
