#include <catch2/catch_test_macros.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <vector>
#include "services/ai/wyoming/wyoming-protocol.hpp"

// NOLINTBEGIN(bugprone-throwing-static-initialization)
using namespace AI::wyoming;

namespace {

QJsonObject header(const QByteArray &encoded) {
  return QJsonDocument::fromJson(encoded.left(encoded.indexOf('\n'))).object();
}

} // namespace

TEST_CASE("wyoming events put data and payload after the header line") {
  const QByteArray payload("\x01\x02\x03\x04", 4);
  const auto encoded = encode("audio-chunk", QJsonObject{{"rate", 16000}}, payload);
  const auto h = header(encoded);

  CHECK(h.value("type").toString() == "audio-chunk");
  CHECK(h.value("payload_length").toInteger() == 4);
  const auto dataLength = h.value("data_length").toInteger();
  const auto body = encoded.mid(encoded.indexOf('\n') + 1);
  CHECK(QJsonDocument::fromJson(body.left(dataLength)).object().value("rate").toInt() == 16000);
  CHECK(body.mid(dataLength) == payload);

  const auto bare = encode("describe");
  CHECK(bare.endsWith('\n'));
  CHECK_FALSE(header(bare).contains("data_length"));
}

TEST_CASE("wyoming parser reassembles events split across reads") {
  const QByteArray payload(3200, '\x7f');
  QByteArray stream = encode("audio-chunk", QJsonObject{{"rate", 16000}}, payload) +
                      encode("transcript", QJsonObject{{"text", "hello world"}});

  EventParser parser;
  std::vector<Event> events;
  for (qsizetype i = 0; i < stream.size(); i += 7) {
    for (auto &event : parser.feed(stream.mid(i, 7))) {
      events.push_back(std::move(event));
    }
  }

  REQUIRE_FALSE(parser.error());
  REQUIRE(events.size() == 2);
  CHECK(events[0].type == "audio-chunk");
  CHECK(events[0].data.value("rate").toInt() == 16000);
  CHECK(events[0].payload == payload);
  CHECK(events[1].type == "transcript");
  CHECK(events[1].data.value("text").toString() == "hello world");
}

TEST_CASE("wyoming parser accepts inline data from older servers") {
  EventParser parser;
  auto events = parser.feed(R"({"type":"transcript","data":{"text":"old style"}})"
                            "\n");
  REQUIRE(events.size() == 1);
  CHECK(events[0].data.value("text").toString() == "old style");
}

TEST_CASE("wyoming parser reports garbage") {
  EventParser parser;
  CHECK(parser.feed("HTTP/1.1 400 Bad Request\r\n").empty());
  CHECK(parser.error());
}

TEST_CASE("wyoming info lists installed speech models") {
  const auto info = QJsonDocument::fromJson(R"({
    "asr": [{
      "name": "parakeet-npu", "installed": true,
      "models": [
        {"name": "nemo-parakeet-tdt-0.6b-v3", "description": "Multilingual ASR", "installed": true,
         "languages": ["en", "es"]},
        {"name": "not-downloaded", "installed": false, "languages": []}
      ]
    }, {
      "name": "disabled", "installed": false,
      "models": [{"name": "hidden", "installed": true}]
    }],
    "tts": []
  })")
                        .object();

  const auto models = parseAsrModels(info);
  REQUIRE(models.size() == 1);
  CHECK(models[0].program == "parakeet-npu");
  CHECK(models[0].name == "nemo-parakeet-tdt-0.6b-v3");
  CHECK(models[0].description == "Multilingual ASR");
  CHECK(models[0].languages == std::vector<std::string>{"en", "es"});
}

TEST_CASE("wyoming transcription requests stream the audio in chunks") {
  const std::vector<std::int16_t> samples(16000 * 25 / 100, 1000); // 250 ms
  const auto request = transcriptionRequest(samples, 16000, "parakeet", "en", 100);

  EventParser parser;
  const auto events = parser.feed(request);
  REQUIRE(events.size() == 6); // transcribe, audio-start, 3 chunks, audio-stop
  CHECK(events[0].type == "transcribe");
  CHECK(events[0].data.value("name").toString() == "parakeet");
  CHECK(events[0].data.value("language").toString() == "en");
  CHECK(events[1].type == "audio-start");
  CHECK(events[1].data.value("width").toInt() == 2);
  CHECK(events[2].payload.size() == 3200);
  CHECK(events[4].payload.size() == 1600);
  CHECK(events[5].type == "audio-stop");

  qsizetype total = 0;
  for (const auto &event : events) {
    total += event.payload.size();
  }
  CHECK(total == static_cast<qsizetype>(samples.size() * 2));

  const auto noLanguage = parser.feed(transcriptionRequest(samples, 16000, std::nullopt, std::nullopt));
  CHECK_FALSE(noLanguage[0].data.contains("language"));
  CHECK_FALSE(noLanguage[0].data.contains("name"));
}

TEST_CASE("wyoming long recordings are split at the quietest moment") {
  constexpr int rate = 16000;
  std::vector<float> audio;
  auto speech = [&](int ms) { audio.insert(audio.end(), static_cast<std::size_t>(rate) * ms / 1000, 0.3f); };
  auto pause = [&](int ms) { audio.insert(audio.end(), static_cast<std::size_t>(rate) * ms / 1000, 0.0f); };

  // 7 s of speech, a pause at 7-7.4 s, then 6 s more: 13.4 s split into pieces of at most 10 s
  speech(7000);
  pause(400);
  speech(6000);

  const auto ends = splitPoints(audio, rate, static_cast<std::size_t>(rate) * 10);
  REQUIRE(ends.size() == 2);
  CHECK(ends[0] > static_cast<std::size_t>(rate) * 7);
  CHECK(ends[0] < static_cast<std::size_t>(rate) * 7400 / 1000);
  CHECK(ends[1] == audio.size());

  // no pause at all still keeps every piece within the limit
  std::vector<float> constant(static_cast<std::size_t>(rate) * 35, 0.3f);
  const auto forced = splitPoints(constant, rate, static_cast<std::size_t>(rate) * 10);
  std::size_t start = 0;
  for (auto end : forced) {
    CHECK(end - start <= static_cast<std::size_t>(rate) * 10);
    CHECK(end > start);
    start = end;
  }
  CHECK(forced.back() == constant.size());

  CHECK(splitPoints(constant, rate, constant.size()) == std::vector<std::size_t>{constant.size()});
}
// NOLINTEND(bugprone-throwing-static-initialization)
