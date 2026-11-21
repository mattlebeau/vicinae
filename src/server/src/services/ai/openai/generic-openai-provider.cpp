#include "services/ai/openai/generic-openai-provider.hpp"
#include <algorithm>
#include <ranges>
#include "services/ai/live/realtime-live-transcriber.hpp"
#include "services/ai/transcription-model-detection.hpp"

namespace AI {

GenericOpenAIProvider::GenericOpenAIProvider(std::string id)
    : OpenAICompatibleProvider(std::move(id), "openai-compatible", "") {}

bool GenericOpenAIProvider::configureExtra(const PreferenceValues &values) {
  const auto fields = readPreferences<GenericOpenAIFields>(values);
  auto models = splitModelList(fields.transcriptionModels);
  const bool modelsChanged = models != m_transcriptionModels;
  m_transcriptionModels = std::move(models);
  m_realtime = fields.realtime;
  m_realtimePath = fields.realtimePath;
  return modelsChanged;
}

Capabilities GenericOpenAIProvider::capabilitiesFor(std::string_view id) const {
  if (std::ranges::contains(m_transcriptionModels, id) || looksLikeTranscriptionModel(id)) {
    return Capability::Transcription;
  }
  return Capability::Completion;
}

QFuture<Result<ModelList>> GenericOpenAIProvider::fetchModels() {
  return OpenAICompatibleProvider::fetchModels().then(this, [this](Result<ModelList> res) -> Result<ModelList> {
    // speech servers like whisper.cpp have no model list, the configured models are all there is then
    if (!res && m_transcriptionModels.empty()) return res;

    ModelList models = res ? std::move(*res) : ModelList{};
    for (const auto &id : m_transcriptionModels) {
      if (std::ranges::contains(models, id, &Model::id)) continue;
      models.emplace_back(Model{.id = id, .name = id, .caps = Capability::Transcription});
    }
    return models;
  });
}

std::unique_ptr<AbstractLiveTranscriber>
GenericOpenAIProvider::createLiveTranscriber(std::string_view modelId, const LiveTranscriptionOptions &opts) {
  if (m_realtime == RealtimeMode::Off || url().empty()) return nullptr;

  const auto dialect = m_realtime == RealtimeMode::Ga ? realtime::Dialect::Ga : realtime::Dialect::Beta;
  RealtimeLiveTranscriber::Endpoint endpoint{
      .url = realtime::endpoint(QString::fromStdString(url()), dialect, std::string(modelId),
                                QString::fromStdString(m_realtimePath)),
      .apiKey = QString::fromStdString(apiKey()),
      .model = std::string(modelId),
      .dialect = dialect,
  };
  return std::make_unique<RealtimeLiveTranscriber>(std::move(endpoint), opts);
}

} // namespace AI
