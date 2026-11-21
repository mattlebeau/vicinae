#include "services/ai/live/realtime-live-transcriber.hpp"
#include <QNetworkRequest>
#include <qlogging.h>

namespace AI {

namespace {
constexpr int SEND_INTERVAL_MS = 100;
// audio recorded before the session is ready is kept, up to this much
constexpr std::size_t MAX_BUFFERED_SAMPLES = static_cast<std::size_t>(realtime::SAMPLE_RATE) * 30;
} // namespace

RealtimeLiveTranscriber::RealtimeLiveTranscriber(Endpoint endpoint, LiveTranscriptionOptions options,
                                                 QObject *parent)
    : AbstractLiveTranscriber(parent), m_endpoint(std::move(endpoint)), m_options(std::move(options)),
      m_resampler(SAMPLE_RATE, realtime::SAMPLE_RATE) {
  connect(&m_socket, &QWebSocket::textMessageReceived, this, &RealtimeLiveTranscriber::handleMessage);
  connect(&m_socket, &QWebSocket::errorOccurred, this,
          [this](QAbstractSocket::SocketError) { fail(m_socket.errorString()); });
  connect(&m_socket, &QWebSocket::disconnected, this, [this] {
    if (!m_stopped) fail(tr("Connection closed (%1)").arg(m_socket.closeReason()));
  });

  m_sendTimer.setInterval(SEND_INTERVAL_MS);
  connect(&m_sendTimer, &QTimer::timeout, this, &RealtimeLiveTranscriber::flushAudio);

  QNetworkRequest request(m_endpoint.url);
  if (!m_endpoint.apiKey.isEmpty()) {
    request.setRawHeader("Authorization", "Bearer " + m_endpoint.apiKey.toUtf8());
  }
  if (m_endpoint.dialect == realtime::Dialect::Beta) request.setRawHeader("OpenAI-Beta", "realtime=v1");

  qInfo() << "Live transcription: connecting to" << m_endpoint.url.toString(QUrl::RemoveQuery);
  m_socket.open(request);
}

RealtimeLiveTranscriber::~RealtimeLiveTranscriber() {
  m_stopped = true;
  m_socket.disconnect(this);
  m_socket.abort();
}

void RealtimeLiveTranscriber::appendAudio(std::span<const float> samples) {
  if (m_stopped) return;
  auto resampled = m_resampler.push(samples);
  m_outgoing.insert(m_outgoing.end(), resampled.begin(), resampled.end());
  if (m_outgoing.size() > MAX_BUFFERED_SAMPLES) {
    m_outgoing.erase(m_outgoing.begin(), m_outgoing.end() - static_cast<std::ptrdiff_t>(MAX_BUFFERED_SAMPLES));
  }
}

void RealtimeLiveTranscriber::flushAudio() {
  if (!m_ready || m_outgoing.empty()) return;
  m_socket.sendTextMessage(QString::fromLatin1(realtime::appendAudio(m_outgoing)));
  m_outgoing.clear();
}

void RealtimeLiveTranscriber::stop() {
  if (m_stopped) return;
  m_stopped = true;
  m_sendTimer.stop();
  m_socket.close();
}

void RealtimeLiveTranscriber::fail(const QString &reason) {
  if (m_stopped) return;
  qWarning() << "Live transcription: realtime session failed:" << reason;
  stop();
  emit failed(reason);
}

void RealtimeLiveTranscriber::handleMessage(const QString &message) {
  if (m_stopped) return;
  using Kind = realtime::TranscriptAssembler::EventKind;

  const auto event = m_assembler.handle(message.toUtf8());
  switch (event.kind) {
  case Kind::SessionCreated:
    m_socket.sendTextMessage(QString::fromUtf8(
        realtime::sessionUpdate(m_endpoint.dialect, m_endpoint.model, m_options.language)));
    m_ready = true;
    flushAudio();
    m_sendTimer.start();
    return;
  case Kind::TextChanged:
    setText(m_assembler.committed(), m_assembler.tentative());
    return;
  case Kind::Error:
    // servers report unsupported session fields this way, which does not stop transcription
    qWarning() << "Live transcription: server error:" << event.message;
    return;
  case Kind::Ignored:
    return;
  }
}

} // namespace AI
