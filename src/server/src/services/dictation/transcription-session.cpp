#include <QDateTime>
#include <QDir>
#include <QFile>
#include <chrono>
#include <qlogging.h>
#include "transcription-session.hpp"
#include "services/ai/ai-service.hpp"
#include "services/dictation/dictation-service.hpp"

namespace {
// Cloud APIs take 25 MB, which is about 13 minutes of what we upload. Going over transcribes what's there.
constexpr auto MAX_RECORDING_DURATION = std::chrono::minutes(12);
constexpr qint64 MIN_SALVAGED_MS = 1000;
} // namespace

TranscriptionSession::TranscriptionSession(DictationService &dictation, QObject *parent)
    : QObject(parent), m_dictation(dictation) {
  m_elapsedTimer.setInterval(1000);

  const auto prepareSoundEffect = [](QSoundEffect &effect, const QUrl &url) {
    effect.setSource(url);
    effect.setLoopCount(1);
    effect.setVolume(0.3f);
  };
  prepareSoundEffect(m_startSound, QUrl(QStringLiteral("qrc:/sound/dictation-start.wav")));
  prepareSoundEffect(m_stopSound, QUrl(QStringLiteral("qrc:/sound/dictation-stop.wav")));

  connect(&m_elapsedTimer, &QTimer::timeout, this, &TranscriptionSession::elapsedTimeChanged);
  m_recordingLimit.setSingleShot(true);
  m_recordingLimit.setInterval(MAX_RECORDING_DURATION);
  connect(&m_recordingLimit, &QTimer::timeout, this, &TranscriptionSession::accept);
  connect(&m_recorder, &Audio::Recorder::levelChanged, this, [this] {
    if (!m_captureReady) {
      m_captureReady = true;
      emit stateChanged();
    }
    emit audioLevelChanged();
  });
  connect(&m_recorder, &Audio::Recorder::stateChanged, this, &TranscriptionSession::stateChanged);
  connect(&m_recorder, &Audio::Recorder::errorOccurred, this, [this]() {
    // what was said before the microphone went away is still worth transcribing
    if (m_recorder.state() == Audio::Recorder::State::Recording &&
        m_recorder.elapsedMs() >= MIN_SALVAGED_MS) {
      accept();
    } else {
      fail(tr("Microphone stopped"));
    }
  });
}

bool TranscriptionSession::start(TranscriptionSetup setup) {
  if (isActive()) return false;
  m_setup = std::move(setup);
  m_captureReady = false;
  m_durationMs = 0;

  m_liveCommitted.clear();
  m_liveTentative.clear();
  m_liveAudio.clear();
  m_recorder.setCaptureObserver(nullptr);

  if (!m_recorder.start()) {
    fail(tr("Could not start recording"));
    return false;
  }

  if (m_setup.liveModel) {
    m_liveResampler =
        std::make_unique<Audio::StreamingResampler>(m_recorder.captureSampleRate(), AI::AbstractLiveTranscriber::SAMPLE_RATE);
    m_recorder.setCaptureObserver([this](std::span<const float> captured) { feedLive(captured); });
    startLive(true);
  }

  if (m_setup.pauseMedia) {
    m_pauseHandle = std::make_unique<MediaControlService::TransientPauseHandle>(
        m_dictation.mediaControl().transientPauseAll());
  }

  m_elapsedTimer.start();
  m_recordingLimit.start();
  emit elapsedTimeChanged();
  if (m_setup.playSoundEffects) m_startSound.play();
  m_dictation.ai().preloadModel(m_setup.model);
  return true;
}

void TranscriptionSession::cancel() {
  if (m_transcribing) {
    ++m_transcriptionId;
    m_transcribing = false;
    emit stateChanged();
    emit cancelled();
    return;
  }
  if (m_recorder.state() == Audio::Recorder::State::Idle) return;
  m_elapsedTimer.stop();
  m_recordingLimit.stop();
  m_recorder.discard();
  stopLive();
  m_pauseHandle.reset();
  m_dictation.ai().cancelPreload(m_setup.model);
  emit stateChanged();
  emit cancelled();
}

void TranscriptionSession::accept() {
  if (m_transcribing || m_recorder.state() == Audio::Recorder::State::Idle) return;

  m_elapsedTimer.stop();
  m_recordingLimit.stop();
  m_durationMs = m_recorder.elapsedMs();
  m_transcribeTimer.start();
  m_recorder.stop();
  // the preview has served its purpose, and must not compete with the final transcription
  stopLive();

  if (m_setup.playSoundEffects) {
    m_startSound.stop();
    m_stopSound.play();
  }
  m_pauseHandle.reset();

  auto recording = m_recorder.finish();
  const auto sampleCount = recording.toF32().size();
  const auto sampleRate = recording.format().sampleRate();
  qInfo() << "Dictation: recorded" << sampleCount << "samples at" << sampleRate << "Hz ("
          << static_cast<double>(sampleCount) * 1000.0 / sampleRate << "ms of audio) over" << m_durationMs
          << "ms";

  if (const auto dir = qEnvironmentVariable("VICINAE_DICTATION_DUMP_DIR"); !dir.isEmpty()) {
    const auto path = QDir(dir).filePath(
        QStringLiteral("dictation-%1.wav").arg(QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss")));
    if (QFile file(path); file.open(QIODevice::WriteOnly)) {
      file.write(recording.toWav());
      qInfo() << "Dictation: dumped recording to" << path;
    }
  }

  if (recording.toF32().empty()) {
    m_dictation.ai().cancelPreload(m_setup.model);
    fail(tr("Nothing to transcribe"));
    return;
  }

  m_transcribing = true;
  emit stateChanged();

  auto options = m_setup.options;
  const auto words = m_dictation.vocabulary()->words();
  m_vocabulary.clear();
  m_vocabulary.reserve(words.size() + m_setup.extraVocabulary.size());
  m_vocabulary.insert(m_vocabulary.end(), words.begin(), words.end());
  m_vocabulary.insert(m_vocabulary.end(), m_setup.extraVocabulary.begin(), m_setup.extraVocabulary.end());
  options.vocabulary = m_vocabulary;

  m_dictation.ai()
      .transcribe(m_setup.model, std::move(recording), options)
      .then(this, [this, id = ++m_transcriptionId](const AI::TranscriptionResult &result) {
        if (id != m_transcriptionId) return;
        qInfo() << "Dictation: transcribed" << m_durationMs << "ms of audio in" << m_transcribeTimer.elapsed()
                << "ms";
        m_transcribing = false;
        emit stateChanged();

        if (!result) {
          qWarning() << "Dictation: transcription failed:" << result.error();
          fail(tr("Transcription failed"));
          return;
        }
        const auto text = QString::fromStdString(result->text).trimmed();
        if (text.isEmpty()) {
          fail(tr("Nothing to transcribe"));
          return;
        }
        emit transcribed({
            .text = text,
            .language = m_setup.options.language ? m_setup.options.language : result->language,
            .durationMs = m_durationMs,
        });
      });
}

QString TranscriptionSession::elapsedTime() const {
  const auto secs = (isRecording() ? m_recorder.elapsedMs() : m_durationMs) / 1000;
  return QStringLiteral("%1:%2")
      .arg(secs / 60, 2, 10, QLatin1Char('0'))
      .arg(secs % 60, 2, 10, QLatin1Char('0'));
}

void TranscriptionSession::fail(const QString &message) {
  m_elapsedTimer.stop();
  m_recordingLimit.stop();
  if (m_recorder.state() != Audio::Recorder::State::Idle) m_recorder.discard();
  stopLive();
  m_pauseHandle.reset();
  emit stateChanged();
  emit failed(message);
}

void TranscriptionSession::startLive(bool allowStreaming) {
  AI::LiveTranscriptionOptions options{.language = m_setup.options.language};
  for (const auto word : m_dictation.vocabulary()->words()) {
    options.vocabulary.emplace_back(word);
  }
  options.vocabulary.insert(options.vocabulary.end(), m_setup.extraVocabulary.begin(), m_setup.extraVocabulary.end());

  m_live = m_dictation.ai().createLiveTranscriber(*m_setup.liveModel, std::move(options), allowStreaming);
  if (!m_live) {
    qWarning() << "Dictation: no live transcription for" << QString::fromStdString(m_setup.liveModel->provider);
    return;
  }
  m_liveStreaming = allowStreaming;

  connect(m_live.get(), &AI::AbstractLiveTranscriber::textChanged, this, [this] {
    m_liveCommitted = m_live->committedText();
    m_liveTentative = m_live->tentativeText();
    emit liveTextChanged();
  });
  connect(m_live.get(), &AI::AbstractLiveTranscriber::failed, this, [this](const QString &reason) {
    qWarning() << "Dictation: live transcription failed:" << reason;
    const bool nothingYet = m_liveCommitted.isEmpty() && m_liveTentative.isEmpty();
    if (m_liveStreaming && nothingYet && isRecording()) {
      // the streaming endpoint is unusable: transcribe in chunks instead, from the start of the recording
      qInfo() << "Dictation: falling back to chunked live transcription";
      m_live.release()->deleteLater();
      startLive(false);
      if (m_live) m_live->appendAudio(m_liveAudio);
    }
  });
}

void TranscriptionSession::stopLive() {
  m_recorder.setCaptureObserver(nullptr);
  if (m_live) {
    m_live->stop();
    m_live->disconnect(this);
    // the failure handler may be running on it
    m_live.release()->deleteLater();
  }
  m_liveResampler.reset();
  m_liveAudio.clear();
  m_liveAudio.shrink_to_fit();
}

void TranscriptionSession::feedLive(std::span<const float> captured) {
  if (!m_liveResampler) return;
  const auto samples = m_liveResampler->push(captured);
  if (samples.empty()) return;
  m_liveAudio.insert(m_liveAudio.end(), samples.begin(), samples.end());
  if (m_live) m_live->appendAudio(samples);
}
