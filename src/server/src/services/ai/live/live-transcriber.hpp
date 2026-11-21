#pragma once
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <QObject>
#include <QString>

namespace AI {

struct LiveTranscriptionOptions {
  std::optional<std::string> language;
  // Words to bias towards, same as for a regular transcription.
  std::vector<std::string> vocabulary;
};

/**
 * Turns audio into text while it is still being recorded, for display only. The text is split into a
 * committed part that will not change anymore and a tentative part that later audio may still revise.
 */
class AbstractLiveTranscriber : public QObject {
  Q_OBJECT

signals:
  void textChanged();
  // The transcriber gave up; the text produced so far stays available.
  void failed(const QString &reason);

public:
  static constexpr int SAMPLE_RATE = 16000;

  explicit AbstractLiveTranscriber(QObject *parent = nullptr) : QObject(parent) {}
  ~AbstractLiveTranscriber() override = default;

  /** Mono 16 kHz float samples, in recording order. */
  virtual void appendAudio(std::span<const float> samples) = 0;

  /** No more audio will come. Pending work may be dropped, the text no longer changes after this. */
  virtual void stop() = 0;

  const QString &committedText() const { return m_committed; }
  const QString &tentativeText() const { return m_tentative; }

protected:
  void setText(QString committed, QString tentative) {
    if (committed == m_committed && tentative == m_tentative) return;
    m_committed = std::move(committed);
    m_tentative = std::move(tentative);
    emit textChanged();
  }

private:
  QString m_committed;
  QString m_tentative;
};

/** Joins transcript pieces with a single space, ignoring empty ones. */
inline QString joinTranscript(const QString &a, const QString &b) {
  const auto left = a.trimmed();
  const auto right = b.trimmed();
  if (left.isEmpty()) return right;
  if (right.isEmpty()) return left;
  return left + QLatin1Char(' ') + right;
}

} // namespace AI
