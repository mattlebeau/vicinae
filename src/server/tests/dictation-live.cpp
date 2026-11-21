#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <cmath>
#include <numbers>
#include <vector>
#include "services/ai/live/chunk-planner.hpp"
#include "services/ai/live/realtime-protocol.hpp"
#include "services/ai/transcription-model-detection.hpp"
#include "services/audio/streaming-resampler.hpp"

// NOLINTBEGIN(bugprone-throwing-static-initialization)
namespace {

constexpr int RATE = 16000;

std::vector<float> tone(int ms, float amplitude = 0.3f, double hz = 220.0) {
  std::vector<float> out(static_cast<std::size_t>(RATE) * ms / 1000);
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / RATE));
  }
  return out;
}

std::vector<float> silence(int ms, float noise = 0.0005f) {
  std::vector<float> out(static_cast<std::size_t>(RATE) * ms / 1000);
  // a faint deterministic hiss, like a real microphone
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = noise * static_cast<float>(((i * 7919) % 200) / 100.0 - 1.0);
  }
  return out;
}

int ms(std::size_t samples) { return static_cast<int>(samples * 1000 / RATE); }

} // namespace

TEST_CASE("chunk planner waits for speech and ignores silence") {
  AI::live::ChunkPlanner planner;
  planner.append(silence(3000));
  REQUIRE_FALSE(planner.next().has_value());
  // only a little lead-in silence is kept around
  REQUIRE(planner.bufferedMs() <= 200);
}

TEST_CASE("chunk planner transcribes a growing chunk tentatively, then commits at a pause") {
  AI::live::ChunkPlanner planner;
  planner.append(silence(1000));
  planner.append(tone(1500));

  auto first = planner.next();
  REQUIRE(first.has_value());
  REQUIRE_FALSE(first->commit);
  // starts at most the lead-in before the speech
  REQUIRE(ms(first->samples.size()) <= 1500 + 220);
  REQUIRE_FALSE(planner.next().has_value()); // one request at a time

  planner.complete(first->id, " hello there ");
  REQUIRE(planner.tentative() == "hello there");
  REQUIRE(planner.committed().empty());

  // not enough new audio yet for another pass
  planner.append(tone(300));
  REQUIRE_FALSE(planner.next().has_value());

  planner.append(tone(600));
  auto second = planner.next();
  REQUIRE(second.has_value());
  REQUIRE_FALSE(second->commit);
  planner.complete(second->id, "hello there friend");

  // more speech the tentative text doesn't cover, then a pause: the chunk is transcribed one last time
  planner.append(tone(400));
  planner.append(silence(800));
  auto third = planner.next();
  REQUIRE(third.has_value());
  REQUIRE(third->commit);
  planner.complete(third->id, "Hello there, friend.");
  REQUIRE(planner.committed() == "Hello there, friend.");
  REQUIRE(planner.tentative().empty());
  REQUIRE(planner.bufferedMs() < 300);

  planner.append(tone(1200));
  auto fourth = planner.next();
  REQUIRE(fourth.has_value());
  planner.append(silence(800));
  planner.complete(fourth->id, "How are you?");
  // the tentative text already covers the whole chunk, so the pause commits it without another request
  REQUIRE_FALSE(planner.next().has_value());
  REQUIRE(planner.committed() == "Hello there, friend. How are you?");
}

TEST_CASE("chunk planner splits long speech at a pause in its second half") {
  AI::live::ChunkPlanner planner(AI::live::ChunkPlannerConfig{.maxChunkMs = 6000});
  planner.append(tone(4000));
  planner.append(silence(300));
  planner.append(tone(2000));

  auto request = planner.next();
  REQUIRE(request.has_value());
  REQUIRE(request->commit);
  const auto length = ms(request->samples.size());
  REQUIRE(length > 4000);
  REQUIRE(length < 4300);
  planner.complete(request->id, "first part");
  REQUIRE(planner.committed() == "first part");
  // what came after the pause is still waiting
  REQUIRE(planner.bufferedMs() > 1900);
}

TEST_CASE("chunk planner tells speech from a noisy room once it heard the room") {
  AI::live::ChunkPlanner planner;
  // loud background, but quieter than the speech
  planner.append(silence(2000, 0.02f));
  REQUIRE_FALSE(planner.next().has_value());
  planner.append(tone(1000, 0.4f));
  auto request = planner.next();
  REQUIRE(request.has_value());
  planner.complete(request->id, "loud room");
  planner.append(silence(800, 0.02f));
  // the pause is recognized against the room noise and commits the chunk
  REQUIRE_FALSE(planner.next().has_value());
  REQUIRE(planner.committed() == "loud room");
}

TEST_CASE("chunk planner commits at a pause that went by while it was busy") {
  AI::live::ChunkPlanner planner;
  planner.append(tone(800));
  auto first = planner.next();
  REQUIRE(first.has_value());

  // a whole sentence, a pause and the start of the next one arrive before the answer
  planner.append(tone(1500));
  planner.append(silence(1000));
  planner.append(tone(900));
  planner.complete(first->id, "One");

  auto second = planner.next();
  REQUIRE(second.has_value());
  REQUIRE(second->commit);
  // ends inside the pause, not at the end of the buffer
  REQUIRE(ms(second->samples.size()) >= 2300);
  REQUIRE(ms(second->samples.size()) <= 2700);
  planner.complete(second->id, "One sentence.");
  REQUIRE(planner.committed() == "One sentence.");
  REQUIRE(planner.bufferedMs() <= 1600);
  REQUIRE(planner.bufferedMs() >= 900);
}

TEST_CASE("chunk planner keeps audio of a failed request") {
  AI::live::ChunkPlanner planner;
  planner.append(tone(1000));
  auto request = planner.next();
  REQUIRE(request.has_value());
  planner.abandon(request->id);
  REQUIRE_FALSE(planner.busy());
  planner.complete(request->id, "late answer"); // stale, ignored
  REQUIRE(planner.tentative().empty());
  REQUIRE(planner.next().has_value());
}

TEST_CASE("transcripts lose non-speech markers") {
  using AI::live::cleanTranscript;
  REQUIRE(cleanTranscript(" [BLANK_AUDIO] ").empty());
  REQUIRE(cleanTranscript("Hello (music) world [Applause]") == "Hello world");
  REQUIRE(cleanTranscript("  spaced\n out  ") == "spaced out");
  REQUIRE(cleanTranscript("unclosed [bracket") == "unclosed [bracket");
}

TEST_CASE("streaming resampler matches the expected length and keeps a tone intact") {
  const int inRate = 48000;
  std::vector<float> input(inRate); // one second at 440 Hz
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / inRate));
  }

  Audio::StreamingResampler whole(inRate, RATE);
  auto expected = whole.push(input);
  const auto tail = whole.flush();
  expected.insert(expected.end(), tail.begin(), tail.end());
  REQUIRE(expected.size() == static_cast<std::size_t>(RATE));

  // the same stream in odd chunks gives the same samples
  Audio::StreamingResampler chunked(inRate, RATE);
  std::vector<float> got;
  for (std::size_t pos = 0; pos < input.size();) {
    const auto n = std::min<std::size_t>(input.size() - pos, 997);
    auto out = chunked.push(std::span(input).subspan(pos, n));
    got.insert(got.end(), out.begin(), out.end());
    pos += n;
  }
  auto rest = chunked.flush();
  got.insert(got.end(), rest.begin(), rest.end());
  REQUIRE(got.size() == expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    REQUIRE(got[i] == Catch::Approx(expected[i]).margin(1e-6));
  }

  // away from the edges the output is the same tone at the new rate
  for (std::size_t i = 200; i < got.size() - 200; i += 37) {
    const auto ideal = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / RATE);
    REQUIRE(got[i] == Catch::Approx(ideal).margin(0.01));
  }
}

TEST_CASE("streaming resampler passes audio through when rates match") {
  Audio::StreamingResampler same(RATE, RATE);
  const std::vector<float> input{0.1f, 0.2f, 0.3f};
  REQUIRE(same.push(input) == input);
  REQUIRE(same.flush().empty());
}

TEST_CASE("realtime endpoints are derived from the HTTP base URL") {
  using AI::realtime::Dialect;
  using AI::realtime::endpoint;

  const auto beta = endpoint("http://192.168.1.20:8000/v1", Dialect::Beta, "Systran/faster-whisper-small");
  REQUIRE(beta.scheme() == "ws");
  REQUIRE(beta.host() == "192.168.1.20");
  REQUIRE(beta.port() == 8000);
  REQUIRE(beta.path() == "/v1/realtime");
  REQUIRE(QUrlQuery(beta).queryItemValue("model") == "Systran/faster-whisper-small");

  const auto ga = endpoint("https://api.openai.com/v1/", Dialect::Ga, "gpt-4o-transcribe");
  REQUIRE(ga.scheme() == "wss");
  REQUIRE(ga.path() == "/v1/realtime");
  REQUIRE(QUrlQuery(ga).queryItemValue("intent") == "transcription");

  REQUIRE(endpoint("http://host/v1", Dialect::Beta, "m", "/ws/stt").path() == "/ws/stt");
  REQUIRE(endpoint("http://host/v1", Dialect::Beta, "m", "live").path() == "/v1/live");
}

TEST_CASE("realtime session updates ask for transcription only") {
  using AI::realtime::Dialect;

  const auto beta = QJsonDocument::fromJson(AI::realtime::sessionUpdate(Dialect::Beta, "whisper", "en")).object();
  REQUIRE(beta["type"].toString() == "session.update");
  const auto betaSession = beta["session"].toObject();
  REQUIRE(betaSession["input_audio_transcription"].toObject()["model"].toString() == "whisper");
  REQUIRE(betaSession["input_audio_transcription"].toObject()["language"].toString() == "en");
  const auto turnDetection = betaSession["turn_detection"].toObject();
  REQUIRE(turnDetection["create_response"].toBool(true) == false);
  // servers like Speaches ignore a turn_detection without every field
  for (const auto *field : {"type", "threshold", "prefix_padding_ms", "silence_duration_ms"}) {
    REQUIRE(turnDetection.contains(QLatin1String(field)));
  }

  const auto ga = QJsonDocument::fromJson(AI::realtime::sessionUpdate(Dialect::Ga, "gpt-4o-transcribe", std::nullopt)).object();
  const auto input = ga["session"].toObject()["audio"].toObject()["input"].toObject();
  REQUIRE(ga["session"].toObject()["type"].toString() == "transcription");
  REQUIRE(input["format"].toObject()["rate"].toInt() == 24000);
  REQUIRE(input["transcription"].toObject()["model"].toString() == "gpt-4o-transcribe");
  REQUIRE_FALSE(input["transcription"].toObject().contains("language"));
}

TEST_CASE("realtime audio is sent as base64 PCM16") {
  const std::vector<float> samples{0.0f, 1.0f, -1.0f, 2.0f};
  const auto event = QJsonDocument::fromJson(AI::realtime::appendAudio(samples)).object();
  REQUIRE(event["type"].toString() == "input_audio_buffer.append");
  const auto pcm = QByteArray::fromBase64(event["audio"].toString().toLatin1());
  REQUIRE(pcm.size() == 8);
  const auto *values = reinterpret_cast<const std::int16_t *>(pcm.constData());
  REQUIRE(values[0] == 0);
  REQUIRE(values[1] == 32767);
  REQUIRE(values[2] == -32767);
  REQUIRE(values[3] == 32767); // clipped
}

TEST_CASE("realtime transcript keeps segments in order and separates tentative text") {
  using Kind = AI::realtime::TranscriptAssembler::EventKind;
  AI::realtime::TranscriptAssembler assembler;

  REQUIRE(assembler.handle(R"({"type":"session.created","session":{}})").kind == Kind::SessionCreated);
  REQUIRE(assembler.handle(R"({"type":"input_audio_buffer.committed","item_id":"a"})").kind == Kind::Ignored);
  REQUIRE(assembler.handle(R"({"type":"input_audio_buffer.committed","item_id":"b"})").kind == Kind::Ignored);

  // the second segment finishes first
  REQUIRE(assembler.handle(R"({"type":"conversation.item.input_audio_transcription.completed","item_id":"b","transcript":"world\n"})").kind == Kind::TextChanged);
  REQUIRE(assembler.committed().isEmpty());
  REQUIRE(assembler.tentative() == "world");

  assembler.handle(R"({"type":"conversation.item.input_audio_transcription.delta","item_id":"a","delta":"Hel"})");
  assembler.handle(R"({"type":"conversation.item.input_audio_transcription.delta","item_id":"a","delta":"lo"})");
  REQUIRE(assembler.tentative() == "Hello world");

  assembler.handle(R"({"type":"conversation.item.input_audio_transcription.completed","item_id":"a","transcript":"Hello,"})");
  REQUIRE(assembler.committed() == "Hello, world");
  REQUIRE(assembler.tentative().isEmpty());

  const auto error = assembler.handle(R"({"type":"error","error":{"message":"unsupported field"}})");
  REQUIRE(error.kind == Kind::Error);
  REQUIRE(error.message == "unsupported field");
  REQUIRE(assembler.handle("not json").kind == Kind::Ignored);
}

TEST_CASE("speech-to-text models are recognized by name") {
  using AI::looksLikeTranscriptionModel;
  REQUIRE(looksLikeTranscriptionModel("Systran/faster-whisper-large-v3"));
  REQUIRE(looksLikeTranscriptionModel("gpt-4o-mini-transcribe"));
  REQUIRE(looksLikeTranscriptionModel("nvidia/parakeet-tdt-0.6b-v2"));
  REQUIRE(looksLikeTranscriptionModel("mistralai/Voxtral-Mini-3B"));
  REQUIRE(looksLikeTranscriptionModel("some-asr-model"));
  REQUIRE_FALSE(looksLikeTranscriptionModel("llama-3.1-8b-instruct"));
  REQUIRE_FALSE(looksLikeTranscriptionModel("qwen2.5-coder"));
  REQUIRE_FALSE(looksLikeTranscriptionModel("mistral-small-latest"));
}

TEST_CASE("configured model lists are split and cleaned") {
  REQUIRE(AI::splitModelList("") == std::vector<std::string>{});
  REQUIRE(AI::splitModelList(" whisper-1 , ,large-v3\nlarge-v3,  tiny ") ==
          std::vector<std::string>{"whisper-1", "large-v3", "tiny"});
}
// NOLINTEND(bugprone-throwing-static-initialization)
