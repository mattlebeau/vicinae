#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <QTimer>
#include <qfuture.h>
#include "services/ai/ai-provider.hpp"
#include "services/ai/ai-provider-types.hpp"
#include "services/ai/wyoming/wyoming-protocol.hpp"

namespace AI {

/**
 * Speech-to-text through a Wyoming server (Home Assistant's voice protocol over TCP), such as
 * wyoming-parakeet-on-intel-npu or wyoming-faster-whisper. Models come from the server's `describe`
 * reply. Each transcription opens a connection, streams the audio and waits for the `transcript`.
 */
class WyomingProvider : public AbstractProvider {
  Q_OBJECT

public:
  explicit WyomingProvider(std::string id);

  std::string id() const override { return m_id; }
  std::string_view type() const override { return m_info.type; }
  std::string displayName() const override { return m_name.empty() ? std::string(m_info.label) : m_name; }
  std::optional<ImageUrl> icon() const override { return ImageUrl{m_info.icon}; }
  std::string_view description() const override { return m_info.description; }
  std::optional<ProviderStatus> status() const override;

  void configure(const PreferenceValues &fields) override;
  void start() override;

  ModelList listModels(const ListModelFilters &filters = {}) const override;
  std::optional<Model> findBestModel(Capabilities caps,
                                     Preference preference = Preference::None) const override;

  std::shared_ptr<AbstractChatCompletionStream> createChatCompletion(std::string_view,
                                                                     const ChatCompletionPayload &) override {
    return nullptr;
  }

  QFuture<TranscriptionResult> transcribe(Audio::Recording recording,
                                          const TranscriptionOptions &opts = {}) override;

private:
  static constexpr auto MIN_RETRY_DELAY = std::chrono::seconds(15);
  static constexpr auto MAX_RETRY_DELAY = std::chrono::minutes(5);
  static constexpr int DESCRIBE_TIMEOUT_MS = 5000;
  // a cold server may compile an encoder for a new audio length before answering
  static constexpr int TRANSCRIBE_TIMEOUT_MS = 180000;

  void describe();
  const wyoming::AsrModel *findModel(std::string_view id) const;

  std::string m_id;
  const ProviderTypeInfo &m_info;
  std::string m_name;
  QString m_host;
  std::uint16_t m_port = 0;
  double m_maxSegmentSeconds = 0;
  std::vector<wyoming::AsrModel> m_models;
  std::optional<std::string> m_error;
  std::uint64_t m_generation = 0;
  QTimer m_retry;
  std::chrono::seconds m_retryDelay = MIN_RETRY_DELAY;
  bool m_started = false;
  bool m_describing = false;
};

} // namespace AI
