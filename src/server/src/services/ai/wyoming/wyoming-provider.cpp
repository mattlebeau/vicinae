#include "services/ai/wyoming/wyoming-provider.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <QPointer>
#include <QPromise>
#include <QTcpSocket>
#include <QUrl>
#include <qlogging.h>

namespace AI {

namespace {

constexpr std::uint16_t DEFAULT_PORT = 10300;
// keep clear of the server's limit, its buckets are counted in feature frames
constexpr double SEGMENT_MARGIN_SECONDS = 0.25;

/**
 * One request/response over a fresh connection: write `request`, then wait for an event of type
 * `expect` (or an `error` event, a disconnect or the timeout). Deletes itself once done.
 */
class Exchange : public QObject {
public:
  using Callback = std::function<void(Result<wyoming::Event>)>;

  static void run(QObject *context, const QString &host, std::uint16_t port, QByteArray request,
                  QString expect, int timeoutMs, Callback callback) {
    auto *exchange = new Exchange(context, std::move(request), std::move(expect), std::move(callback));
    exchange->m_timer.start(timeoutMs);
    exchange->m_socket.connectToHost(host, port);
  }

private:
  Exchange(QObject *parent, QByteArray request, QString expect, Callback callback)
      : QObject(parent), m_request(std::move(request)), m_expect(std::move(expect)),
        m_callback(std::move(callback)) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this,
            [this] { finish(std::unexpected("Wyoming server timed out")); });
    connect(&m_socket, &QTcpSocket::connected, this, [this] { m_socket.write(m_request); });
    connect(&m_socket, &QTcpSocket::readyRead, this, &Exchange::read);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
      read(); // the server may close right after its answer
      finish(std::unexpected(m_socket.errorString().toStdString()));
    });
  }

  void read() {
    for (auto &event : m_parser.feed(m_socket.readAll())) {
      if (event.type == m_expect) {
        finish(std::move(event));
        return;
      }
      if (event.type == QLatin1String("error")) {
        finish(std::unexpected("Wyoming server error: " + event.data.value("text").toString().toStdString()));
        return;
      }
    }
    if (m_parser.error()) finish(std::unexpected(m_parser.error()->toStdString()));
  }

  void finish(Result<wyoming::Event> result) {
    if (m_done) return;
    m_done = true;
    m_timer.stop();
    m_socket.abort();
    deleteLater();
    m_callback(std::move(result));
  }

  QTcpSocket m_socket{this};
  QTimer m_timer{this};
  wyoming::EventParser m_parser;
  QByteArray m_request;
  QString m_expect;
  Callback m_callback;
  bool m_done = false;
};

/**
 * Transcribes the pieces of a recording one after the other and joins their text.
 */
struct Job {
  QPromise<TranscriptionResult> promise;
  std::vector<std::int16_t> pcm;
  std::vector<std::size_t> ends;
  std::size_t next = 0;
  int rate = 16000;
  std::optional<std::string> model;
  std::optional<std::string> language;
  std::string text;
};

void runPiece(QObject *context, const QString &host, std::uint16_t port, const std::shared_ptr<Job> &job,
              int timeoutMs) {
  const std::size_t begin = job->next == 0 ? 0 : job->ends[job->next - 1];
  const std::size_t end = job->ends[job->next];
  auto request = wyoming::transcriptionRequest(std::span(job->pcm).subspan(begin, end - begin), job->rate,
                                               job->model, job->language);

  Exchange::run(context, host, port, std::move(request), QStringLiteral("transcript"), timeoutMs,
                [context = QPointer(context), host, port, job, timeoutMs](Result<wyoming::Event> result) {
                  if (!result) {
                    job->promise.addResult(std::unexpected(result.error()));
                    job->promise.finish();
                    return;
                  }
                  auto piece = result->data.value("text").toString().trimmed().toStdString();
                  if (!piece.empty()) {
                    if (!job->text.empty()) job->text += ' ';
                    job->text += piece;
                  }
                  if (++job->next < job->ends.size() && context) {
                    runPiece(context, host, port, job, timeoutMs);
                    return;
                  }
                  job->promise.addResult(TranscriptionResponse{.text = job->text});
                  job->promise.finish();
                });
}

} // namespace

WyomingProvider::WyomingProvider(std::string id) : m_id(std::move(id)), m_info(*findProviderType("wyoming")) {
  m_retry.setSingleShot(true);
  connect(&m_retry, &QTimer::timeout, this, &WyomingProvider::describe);
}

void WyomingProvider::configure(const PreferenceValues &values) {
  auto fields = readPreferences<WyomingFields>(values);
  m_name = std::move(fields.name);
  m_maxSegmentSeconds =
      std::isfinite(fields.maxSegmentSeconds) ? std::max(0.0, fields.maxSegmentSeconds) : 0.0;

  auto text = QString::fromStdString(fields.url).trimmed();
  if (text.isEmpty()) text = QStringLiteral("tcp://localhost:10300");
  if (!text.contains(QLatin1String("://"))) text.prepend(QLatin1String("tcp://"));
  const QUrl url(text);
  const auto host = url.host();
  const auto port = static_cast<std::uint16_t>(url.port(DEFAULT_PORT));

  if (host == m_host && port == m_port) return;
  m_host = host;
  m_port = port;
  qInfo() << "AI provider" << m_id << "now targets Wyoming server" << m_host << m_port;

  if (!m_started) return;
  m_models.clear();
  m_error.reset();
  m_retry.stop();
  m_retryDelay = MIN_RETRY_DELAY;
  emit modelsUpdated();
  describe();
}

void WyomingProvider::start() {
  m_started = true;
  describe();
}

void WyomingProvider::describe() {
  if (m_host.isEmpty()) {
    m_error = "No server address";
    return;
  }
  const auto generation = ++m_generation;
  m_describing = true;

  Exchange::run(this, m_host, m_port, wyoming::encode("describe"), QStringLiteral("info"),
                DESCRIBE_TIMEOUT_MS, [this, generation](Result<wyoming::Event> result) {
                  if (generation != m_generation) return; // the address changed meanwhile
                  m_describing = false;

                  if (result) {
                    m_models = wyoming::parseAsrModels(result->data);
                    m_error = m_models.empty()
                                  ? std::optional<std::string>("The server offers no speech-to-text model")
                                  : std::nullopt;
                  } else {
                    m_error = result.error();
                  }

                  if (m_error) {
                    qWarning() << "AI provider" << m_id << "could not list Wyoming models:" << *m_error
                               << "- retrying in" << m_retryDelay.count() << "s";
                    m_retry.start(m_retryDelay);
                    m_retryDelay = std::min<std::chrono::seconds>(m_retryDelay * 2, MAX_RETRY_DELAY);
                  } else {
                    m_retryDelay = MIN_RETRY_DELAY;
                  }
                  emit modelsUpdated();
                });
}

std::optional<ProviderStatus> WyomingProvider::status() const {
  if (!m_models.empty()) return std::nullopt;
  if (m_describing && !m_error) {
    return ProviderStatus{.kind = ProviderStatus::Kind::Busy, .message = "Connecting to the Wyoming server"};
  }
  if (!m_error) return std::nullopt;
  return ProviderStatus{
      .kind = ProviderStatus::Kind::Error,
      .message =
          QStringLiteral("Can't reach the Wyoming server at %1:%2").arg(m_host).arg(m_port).toStdString(),
      .details = m_error,
  };
}

const wyoming::AsrModel *WyomingProvider::findModel(std::string_view id) const {
  auto it = std::ranges::find(m_models, id, &wyoming::AsrModel::name);
  return it == m_models.end() ? nullptr : &*it;
}

ModelList WyomingProvider::listModels(const ListModelFilters &filters) const {
  if (filters.caps && !(*filters.caps & Capability::Transcription)) return {};

  ModelList models;
  for (const auto &asr : m_models) {
    if (std::ranges::contains(models, asr.name, &Model::id)) continue;
    std::string description = asr.description;
    if (!asr.program.empty()) description += (description.empty() ? "" : " · ") + asr.program;
    models.push_back(Model{
        .id = asr.name,
        .name = asr.name,
        .description = description.empty() ? std::nullopt : std::optional(description),
        .icon = ImageUrl{BuiltinIcon::Waveform},
        .caps = Capability::Transcription,
    });
  }
  return models;
}

std::optional<Model> WyomingProvider::findBestModel(Capabilities caps, Preference preference) const {
  if (preference != Preference::None) return std::nullopt;
  auto models = listModels({.caps = caps});
  if (models.empty()) return std::nullopt;
  return models.front();
}

QFuture<TranscriptionResult> WyomingProvider::transcribe(Audio::Recording recording,
                                                         const TranscriptionOptions &opts) {
  if (m_host.isEmpty()) {
    return QtFuture::makeReadyValueFuture<TranscriptionResult>(std::unexpected("No Wyoming server address"));
  }

  auto job = std::make_shared<Job>();
  job->model = opts.model;
  if (!job->model && !m_models.empty()) job->model = m_models.front().name;

  // only pass languages the model knows; the server's default is better than a rejected request
  if (opts.language) {
    const auto *model = job->model ? findModel(*job->model) : nullptr;
    if (!model || model->languages.empty() || std::ranges::contains(model->languages, *opts.language)) {
      job->language = opts.language;
    }
  }

  const auto format = recording.format();
  const int channels = std::max(1, format.channelCount());
  job->rate = format.sampleRate() > 0 ? format.sampleRate() : 16000;

  const auto interleaved = recording.toF32();
  std::vector<float> mono(interleaved.size() / static_cast<std::size_t>(channels));
  for (std::size_t i = 0; i < mono.size(); ++i) {
    float sum = 0;
    for (int c = 0; c < channels; ++c) {
      sum += interleaved[(i * static_cast<std::size_t>(channels)) + static_cast<std::size_t>(c)];
    }
    mono[i] = sum / static_cast<float>(channels);
  }

  job->pcm.reserve(mono.size());
  for (float sample : mono) {
    job->pcm.push_back(static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)));
  }

  if (m_maxSegmentSeconds > 0) {
    const double seconds = std::max(1.0, m_maxSegmentSeconds - SEGMENT_MARGIN_SECONDS);
    job->ends = wyoming::splitPoints(mono, job->rate, static_cast<std::size_t>(seconds * job->rate));
  } else {
    job->ends = {mono.size()};
  }
  if (job->ends.size() > 1) {
    qInfo() << "Wyoming: transcribing" << mono.size() / job->rate << "s of audio in" << job->ends.size()
            << "pieces";
  }

  job->promise.start();
  auto future = job->promise.future();
  runPiece(this, m_host, m_port, job, TRANSCRIBE_TIMEOUT_MS);
  return future;
}

} // namespace AI
