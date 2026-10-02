#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

class QAudioSink;
class QBuffer;

// Short, quiet UI cues synthesized in memory. One instance is shared by every
// window of the process; failures (no device, disabled env) stay silent.
class SoundEffects : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // App-wide "Reduce motion" preference (QSettings motion/reduced), shared by
    // every window with the sound preference; it lives here so one object owns
    // all persisted feedback preferences.
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
public:
    // settingsPath: optional INI file (tests); empty uses the application's QSettings.
    // audioOutput: false keeps all logic but never touches an audio device.
    explicit SoundEffects(QObject *parent = nullptr, const QString &settingsPath = {}, bool audioOutput = true);
    ~SoundEffects() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    bool reduceMotion() const { return m_reduceMotion; }
    void setReduceMotion(bool reduce);
    // OMAWORD_SOUND=0 (exported by bin/test) makes play() a no-op; the offscreen
    // platform only suppresses the audio device.
    bool silenced() const { return m_silenced; }
    static QStringList names();
    static constexpr int cooldownMs = 50;

    // Returns true when the cue was accepted (enabled, known, not silenced, past cooldown).
    Q_INVOKABLE bool play(const QString &name);

signals:
    void enabledChanged();
    void reduceMotionChanged();

private:
    bool ensureOutput();
    QString m_settingsPath;
    bool m_enabled = true;
    bool m_reduceMotion = false;
    bool m_silenced = false;
    bool m_audioOutput = true;
    bool m_outputFailed = false;
    QHash<QString, QByteArray> m_tones;
    QHash<QString, QElapsedTimer> m_lastPlayed;
    QAudioSink *m_sink = nullptr;
    QBuffer *m_buffer = nullptr;
};
