#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace AI {

/**
 * Whether a model id served by an unknown OpenAI-compatible server names a speech-to-text model.
 */
inline bool looksLikeTranscriptionModel(std::string_view id) {
  static constexpr auto MARKERS = std::to_array<std::string_view>(
      {"whisper", "transcribe", "parakeet", "voxtral", "canary", "moonshine", "sensevoice", "speech-to-text",
       "wav2vec", "-asr", "asr-", "-stt", "stt-"});
  std::string lower(id);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
  return std::ranges::any_of(MARKERS, [&](std::string_view marker) { return lower.contains(marker); });
}

/** Splits a comma or newline separated list of model ids, dropping blanks and duplicates. */
inline std::vector<std::string> splitModelList(std::string_view list) {
  std::vector<std::string> models;
  const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  std::size_t start = 0;
  while (start <= list.size()) {
    const auto end = list.find_first_of(",\n", start);
    auto item = list.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    while (!item.empty() && isSpace(item.front())) item.remove_prefix(1);
    while (!item.empty() && isSpace(item.back())) item.remove_suffix(1);
    if (!item.empty() && !std::ranges::contains(models, item)) models.emplace_back(item);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return models;
}

} // namespace AI
