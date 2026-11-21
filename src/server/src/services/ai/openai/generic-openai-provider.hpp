#pragma once
#include <string>
#include <vector>
#include "services/ai/ai-provider-types.hpp"
#include "services/ai/openai/openai-compatible-provider.hpp"

namespace AI {

/**
 * Any OpenAI-compatible server. Besides chat models it can serve speech-to-text models, detected by name
 * or listed by the user, and stream live transcriptions over the Realtime API.
 */
class GenericOpenAIProvider : public OpenAICompatibleProvider {
  Q_OBJECT

public:
  explicit GenericOpenAIProvider(std::string id);

  std::unique_ptr<AbstractLiveTranscriber> createLiveTranscriber(std::string_view modelId,
                                                                 const LiveTranscriptionOptions &opts) override;

protected:
  bool configureExtra(const PreferenceValues &values) override;
  Capabilities capabilitiesFor(std::string_view id) const override;
  QFuture<Result<ModelList>> fetchModels() override;

private:
  std::vector<std::string> m_transcriptionModels;
  RealtimeMode m_realtime = RealtimeMode::Off;
  std::string m_realtimePath;
};

} // namespace AI
