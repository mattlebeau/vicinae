#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace AI::live {

struct ChunkPlannerConfig {
  int sampleRate = 16000;
  int frameMs = 20;
  // never transcribe less than this
  int minAudioMs = 600;
  // new audio needed before a chunk that is still growing is transcribed again
  int minNewAudioMs = 700;
  // trailing silence that ends a chunk
  int pauseMs = 600;
  // a chunk is committed at the best pause it has once it gets this long
  int maxChunkMs = 20000;
  // silence kept in front of speech so the first word isn't clipped
  int leadInMs = 200;
  // a frame is speech when it is this much louder than the noise floor
  double speechAboveFloorDb = 14.0;
  double minSpeechDb = -60.0;
  // the noise floor is never assumed louder than this, so speech that starts right away isn't taken for noise
  double maxNoiseFloorDb = -45.0;
};

/**
 * Decides which part of a growing recording to transcribe next for a live preview. The recording is cut
 * into chunks at pauses: a chunk is transcribed again as it grows (tentative text) and committed once a
 * pause or the length limit ends it, after which its audio is dropped.
 */
class ChunkPlanner {
public:
  struct Request {
    std::uint64_t id = 0;
    std::vector<float> samples;
    // whether the text ends the chunk
    bool commit = false;
  };

  explicit ChunkPlanner(ChunkPlannerConfig config = {});

  void append(std::span<const float> samples);

  /** What to transcribe now, if anything. Never returns a request while one is pending. */
  std::optional<Request> next();
  void complete(std::uint64_t id, std::string_view text);
  /** The request failed, its audio stays queued. */
  void abandon(std::uint64_t id);

  bool busy() const { return m_pending.has_value(); }
  const std::string &committed() const { return m_committed; }
  const std::string &tentative() const { return m_tentative; }
  /** Audio waiting in the current chunk, in milliseconds. */
  int bufferedMs() const;
  double noiseFloorDb() const;

private:
  struct Pending {
    std::uint64_t id;
    std::size_t frames;
    bool commit;
  };

  std::size_t frameSamples() const;
  bool isSpeech(double db, double floor) const;
  void dropFrames(std::size_t frames);
  void commitText(std::string_view text);
  Request makeRequest(std::size_t frames, bool commit);

  ChunkPlannerConfig m_config;
  std::vector<float> m_samples;
  // loudness of each complete frame of m_samples
  std::vector<double> m_frameDb;
  // recent frame loudness across chunks, for the noise floor
  std::vector<double> m_history;
  std::optional<Pending> m_pending;
  std::uint64_t m_nextId = 1;
  // frames the current tentative text was transcribed from
  std::size_t m_tentativeFrames = 0;
  std::string m_committed;
  std::string m_tentative;
};

/** Removes what whisper-style models emit for non-speech, such as "[BLANK_AUDIO]" or "(music)". */
std::string cleanTranscript(std::string_view text);

} // namespace AI::live
