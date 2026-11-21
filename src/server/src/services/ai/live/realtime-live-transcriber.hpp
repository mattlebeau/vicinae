#pragma once
#include <QTimer>
#include <QWebSocket>
#include <optional>
#include <string>
#include <vector>
#include "services/ai/live/live-transcriber.hpp"
#include "services/ai/live/realtime-protocol.hpp"
#include "services/audio/streaming-resampler.hpp"

namespace AI {

/**
 * Live transcription over the OpenAI Realtime WebSocket API: audio is streamed as it is recorded and the
 * server sends text back for each segment of speech it detects.
 */
class RealtimeLiveTranscriber : public AbstractLiveTranscriber {
  Q_OBJECT

public:
  struct Endpoint {
    QUrl url;
    QString apiKey;
    std::string model;
    realtime::Dialect dialect = realtime::Dialect::Beta;
  };

  RealtimeLiveTranscriber(Endpoint endpoint, LiveTranscriptionOptions options, QObject *parent = nullptr);
  ~RealtimeLiveTranscriber() override;

  void appendAudio(std::span<const float> samples) override;
  void stop() override;

private:
  void handleMessage(const QString &message);
  void flushAudio();
  void fail(const QString &reason);

  Endpoint m_endpoint;
  LiveTranscriptionOptions m_options;
  QWebSocket m_socket;
  Audio::StreamingResampler m_resampler;
  realtime::TranscriptAssembler m_assembler;
  std::vector<float> m_outgoing;
  QTimer m_sendTimer;
  bool m_ready = false;
  bool m_stopped = false;
};

} // namespace AI
