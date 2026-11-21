#include "services/ai/live/live-transcriber.hpp"
#include "services/ai/ai-provider.hpp"

namespace AI {

std::unique_ptr<AbstractLiveTranscriber> AbstractProvider::createLiveTranscriber(std::string_view,
                                                                                 const LiveTranscriptionOptions &) {
  return nullptr;
}

} // namespace AI
