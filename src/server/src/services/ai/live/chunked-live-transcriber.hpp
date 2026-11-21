#pragma once
#include <QTimer>
#include <string>
#include <string_view>
#include <vector>
#include "services/ai/ai-provider.hpp"
#include "services/ai/live/chunk-planner.hpp"
#include "services/ai/live/live-transcriber.hpp"

namespace AI {

class Service;

/**
 * Live transcription for any model that only transcribes whole recordings: the planner cuts the audio
 * into chunks at pauses and each chunk is transcribed again while it grows.
 */
class ChunkedLiveTranscriber : public AbstractLiveTranscriber {
  Q_OBJECT

public:
  ChunkedLiveTranscriber(Service &ai, ModelRef model, LiveTranscriptionOptions options,
                         QObject *parent = nullptr);

  void appendAudio(std::span<const float> samples) override;
  void stop() override;

private:
  void tick();
  void publish();

  Service &m_ai;
  ModelRef m_model;
  LiveTranscriptionOptions m_options;
  std::vector<std::string_view> m_vocabulary;
  live::ChunkPlanner m_planner;
  QTimer m_timer;
  int m_failures = 0;
  bool m_stopped = false;
};

} // namespace AI
