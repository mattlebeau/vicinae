#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <optional>
#include "services/dictation/dictation-session.hpp"
#include "ui/windows/hud-activity.hpp"
#include "ui/image/image-url.hpp"
#include "ui/qml-engine-scope.hpp"

class QQuickWindow;

class HudBridge : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Hud)
  QML_SINGLETON

public:
  static HudBridge *create(QQmlEngine *engine, QJSEngine *) { return QmlEngineScope::get<HudBridge>(engine); }

private:
  Q_PROPERTY(bool visible READ visible NOTIFY visibleChanged)
  Q_PROPERTY(QString text READ text NOTIFY contentChanged)
  Q_PROPERTY(ImageUrl icon READ icon NOTIFY contentChanged)
  Q_PROPERTY(bool hasIcon READ hasIcon NOTIFY contentChanged)
  Q_PROPERTY(DictationSession *dictation READ dictation NOTIFY dictationChanged)
  Q_PROPERTY(HudActivity *activity READ activity NOTIFY activityChanged)
  Q_PROPERTY(double scale READ scale NOTIFY scaleChanged)

public:
  explicit HudBridge(QObject *parent);

  bool visible() const { return m_visible; }
  QString text() const { return m_text; }
  ImageUrl icon() const { return m_icon; }
  bool hasIcon() const { return m_icon.isValid(); }
  DictationSession *dictation() const { return m_dictation; }
  HudActivity *activity() const { return m_activity; }
  /** Size of the dictation HUD relative to its default. Notices are never scaled. */
  double scale() const;

  Q_INVOKABLE void registerWindow(QQuickWindow *window);

public slots:
  void show(const QString &title, const std::optional<ImageURL> &icon);
  void showDictation(DictationSession *session);
  void showActivity(HudActivity *activity);

signals:
  void visibleChanged();
  void contentChanged();
  void dictationChanged();
  void activityChanged();
  void scaleChanged();

private:
  struct Message {
    QString text;
    std::optional<ImageURL> icon;
  };

  void hide();
  void clearDictation();
  void clearActivity();
  void showWindow();

  bool m_visible = false;
  QString m_text;
  ImageUrl m_icon;
  QTimer m_timer;
  // VICINAE_HUD_CAPTURE_DIR saves what the HUD shows, for checking its layout without a screenshot tool
  QTimer m_captureTimer;
  QString m_captureDir;
  int m_captureCount = 0;
  QPointer<QQuickWindow> m_window;
  QPointer<DictationSession> m_dictation;
  QPointer<HudActivity> m_activity;
  std::optional<Message> m_pendingMessage;
};
