#include "services/ai/live/chunked-live-transcriber.hpp"
#include <qlogging.h>
#include "services/ai/ai-service.hpp"

namespace AI {

namespace {
constexpr int TICK_MS = 250;
constexpr int MAX_FAILURES = 3;
// models drop the last word when speech runs straight into the end of the audio
constexpr int TRAILING_SILENCE_MS = 300;
constexpr qsizetype CONTEXT_CHARS = 200;
} // namespace

ChunkedLiveTranscriber::ChunkedLiveTranscriber(Service &ai, ModelRef model, LiveTranscriptionOptions options,
                                               QObject *parent)
    : AbstractLiveTranscriber(parent), m_ai(ai), m_model(std::move(model)), m_options(std::move(options)),
      m_planner(live::ChunkPlannerConfig{.sampleRate = SAMPLE_RATE}) {
  m_vocabulary.reserve(m_options.vocabulary.size());
  for (const auto &word : m_options.vocabulary) {
    m_vocabulary.emplace_back(word);
  }
  m_timer.setInterval(TICK_MS);
  connect(&m_timer, &QTimer::timeout, this, &ChunkedLiveTranscriber::tick);
  m_timer.start();
}

void ChunkedLiveTranscriber::appendAudio(std::span<const float> samples) {
  if (!m_stopped) m_planner.append(samples);
}

void ChunkedLiveTranscriber::stop() {
  m_stopped = true;
  m_timer.stop();
}

void ChunkedLiveTranscriber::publish() {
  setText(QString::fromStdString(m_planner.committed()), QString::fromStdString(m_planner.tentative()));
}

void ChunkedLiveTranscriber::tick() {
  if (m_stopped) return;

  auto request = m_planner.next();
  publish();
  if (!request) return;

  auto samples = std::move(request->samples);
  samples.insert(samples.end(), static_cast<std::size_t>(SAMPLE_RATE) * TRAILING_SILENCE_MS / 1000, 0.0f);

  QAudioFormat format;
  format.setSampleRate(SAMPLE_RATE);
  format.setChannelCount(1);
  format.setSampleFormat(QAudioFormat::Float);

  TranscriptionOptions options{.language = m_options.language, .vocabulary = m_vocabulary};
  if (const auto &committed = m_planner.committed(); !committed.empty()) {
    options.context = QString::fromStdString(committed).right(CONTEXT_CHARS).toStdString();
  }

  m_ai.transcribe(m_model, Audio::Recording(std::move(samples), format), options)
      .then(this, [this, id = request->id](const Result<TranscriptionResponse> &result) {
        if (m_stopped) return;
        if (!result) {
          qWarning() << "Live transcription: chunk failed:" << result.error();
          m_planner.abandon(id);
          if (++m_failures >= MAX_FAILURES) {
            stop();
            emit failed(QString::fromStdString(result.error()));
          }
          return;
        }
        m_failures = 0;
        m_planner.complete(id, result->text);
        publish();
      });
}

} // namespace AI
