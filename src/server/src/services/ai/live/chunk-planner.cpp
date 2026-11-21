#include "services/ai/live/chunk-planner.hpp"
#include <algorithm>
#include <cmath>

namespace AI::live {

namespace {

constexpr std::size_t HISTORY_FRAMES = 500;
// the quietest tenth of recent frames is taken as background noise
constexpr double FLOOR_PERCENTILE = 0.1;
constexpr int MIN_SPLIT_SILENCE_MS = 200;

std::string_view trim(std::string_view s) {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isSpace(s.back())) s.remove_suffix(1);
  return s;
}

void appendJoined(std::string &out, std::string_view text) {
  text = trim(text);
  if (text.empty()) return;
  if (!out.empty()) out += ' ';
  out += text;
}

} // namespace

std::string cleanTranscript(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '[' || c == '(') {
      const char close = c == '[' ? ']' : ')';
      if (const auto end = text.find(close, i); end != std::string_view::npos) {
        i = end + 1;
        continue;
      }
    }
    out += c;
    ++i;
  }

  // collapse the whitespace the removals leave behind
  std::string collapsed;
  collapsed.reserve(out.size());
  bool space = false;
  for (const char ch : trim(out)) {
    const bool isSpace = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    if (isSpace) {
      space = true;
      continue;
    }
    if (space && !collapsed.empty()) collapsed += ' ';
    space = false;
    collapsed += ch;
  }
  return collapsed;
}

ChunkPlanner::ChunkPlanner(ChunkPlannerConfig config) : m_config(config) {}

std::size_t ChunkPlanner::frameSamples() const {
  return static_cast<std::size_t>(m_config.sampleRate) * m_config.frameMs / 1000;
}

int ChunkPlanner::bufferedMs() const { return static_cast<int>(m_frameDb.size()) * m_config.frameMs; }

void ChunkPlanner::append(std::span<const float> samples) {
  const auto frame = frameSamples();
  const auto before = m_frameDb.size() * frame;
  m_samples.insert(m_samples.end(), samples.begin(), samples.end());

  for (auto start = before; start + frame <= m_samples.size(); start += frame) {
    double sum = 0.0;
    for (std::size_t i = start; i < start + frame; ++i) {
      sum += static_cast<double>(m_samples[i]) * m_samples[i];
    }
    const double db = 10.0 * std::log10(std::max(sum / static_cast<double>(frame), 1e-12));
    m_frameDb.emplace_back(db);
    m_history.emplace_back(db);
  }

  if (m_history.size() > 2 * HISTORY_FRAMES) {
    m_history.erase(m_history.begin(), m_history.end() - HISTORY_FRAMES);
  }
}

double ChunkPlanner::noiseFloorDb() const {
  if (m_history.empty()) return m_config.maxNoiseFloorDb;
  const auto count = std::min(m_history.size(), HISTORY_FRAMES);
  std::vector<double> recent(m_history.end() - static_cast<std::ptrdiff_t>(count), m_history.end());
  const auto nth = recent.begin() + static_cast<std::ptrdiff_t>(static_cast<double>(count - 1) * FLOOR_PERCENTILE);
  std::nth_element(recent.begin(), nth, recent.end());
  return std::min(*nth, m_config.maxNoiseFloorDb);
}

bool ChunkPlanner::isSpeech(double db, double floor) const {
  return db > std::max(m_config.minSpeechDb, floor + m_config.speechAboveFloorDb);
}

void ChunkPlanner::dropFrames(std::size_t frames) {
  frames = std::min(frames, m_frameDb.size());
  const auto samples = std::min(frames * frameSamples(), m_samples.size());
  m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(samples));
  m_frameDb.erase(m_frameDb.begin(), m_frameDb.begin() + static_cast<std::ptrdiff_t>(frames));
  m_tentativeFrames = m_tentativeFrames > frames ? m_tentativeFrames - frames : 0;
}

void ChunkPlanner::commitText(std::string_view text) {
  appendJoined(m_committed, cleanTranscript(text));
  m_tentative.clear();
  m_tentativeFrames = 0;
}

ChunkPlanner::Request ChunkPlanner::makeRequest(std::size_t frames, bool commit) {
  const auto id = m_nextId++;
  m_pending = Pending{.id = id, .frames = frames, .commit = commit};
  const auto samples = frames * frameSamples();
  return Request{
      .id = id,
      .samples = std::vector<float>(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(samples)),
      .commit = commit,
  };
}

std::optional<ChunkPlanner::Request> ChunkPlanner::next() {
  if (m_pending) return std::nullopt;

  const auto msToFrames = [&](int ms) { return static_cast<std::size_t>(ms / m_config.frameMs); };
  const double floor = noiseFloorDb();
  const auto frames = m_frameDb.size();
  if (frames == 0) return std::nullopt;

  const auto first = std::ranges::find_if(m_frameDb, [&](double db) { return isSpeech(db, floor); });
  if (first == m_frameDb.end()) {
    // nothing said yet: keep only a little silence as lead-in for what comes next
    if (frames > msToFrames(m_config.leadInMs)) dropFrames(frames - msToFrames(m_config.leadInMs));
    return std::nullopt;
  }

  // the chunk starts shortly before its first speech
  const auto firstSpeech = static_cast<std::size_t>(first - m_frameDb.begin());
  if (const auto leadIn = msToFrames(m_config.leadInMs); firstSpeech > leadIn) {
    dropFrames(firstSpeech - leadIn);
  }

  const auto count = m_frameDb.size();
  if (count < msToFrames(m_config.minAudioMs)) return std::nullopt;

  // a pause that went by while a request was pending still ends the chunk there
  const auto pauseFrames = msToFrames(m_config.pauseMs);
  std::size_t run = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (!isSpeech(m_frameDb[i], floor)) {
      ++run;
      continue;
    }
    const auto runStart = i - run;
    if (run >= pauseFrames && runStart > 0) {
      const auto split = runStart + pauseFrames / 2;
      if (!m_tentative.empty() && m_tentativeFrames >= runStart && m_tentativeFrames <= split) {
        commitText(m_tentative);
        dropFrames(split);
        return std::nullopt;
      }
      return makeRequest(split, true);
    }
    run = 0;
  }

  std::size_t lastSpeech = count - 1;
  while (lastSpeech > 0 && !isSpeech(m_frameDb[lastSpeech], floor)) --lastSpeech;
  const auto trailing = count - 1 - lastSpeech;

  if (trailing >= msToFrames(m_config.pauseMs)) {
    // the speaker paused: the chunk is over
    if (!m_tentative.empty() && m_tentativeFrames > lastSpeech) {
      // the tentative text already covers everything that was said
      const auto covered = m_tentativeFrames;
      commitText(m_tentative);
      dropFrames(covered);
      return std::nullopt;
    }
    return makeRequest(count, true);
  }

  if (count >= msToFrames(m_config.maxChunkMs)) {
    // split at the longest late pause, or everything if there is none
    std::size_t best = count;
    std::size_t bestRun = 0;
    std::size_t run = 0;
    for (std::size_t i = count / 2; i < count; ++i) {
      run = isSpeech(m_frameDb[i], floor) ? 0 : run + 1;
      if (run >= msToFrames(MIN_SPLIT_SILENCE_MS) && run >= bestRun) {
        bestRun = run;
        best = i + 1 - run / 2;
      }
    }
    return makeRequest(best, true);
  }

  if (count < m_tentativeFrames + msToFrames(m_config.minNewAudioMs)) return std::nullopt;
  return makeRequest(count, false);
}

void ChunkPlanner::complete(std::uint64_t id, std::string_view text) {
  if (!m_pending || m_pending->id != id) return;
  const auto pending = *m_pending;
  m_pending.reset();

  if (pending.commit) {
    commitText(text);
    dropFrames(pending.frames);
  } else {
    m_tentative = cleanTranscript(text);
    m_tentativeFrames = pending.frames;
  }
}

void ChunkPlanner::abandon(std::uint64_t id) {
  if (m_pending && m_pending->id == id) m_pending.reset();
}

} // namespace AI::live
