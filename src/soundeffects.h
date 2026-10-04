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
    // Test hook: the last cue requested while enabled (even when silenced or
    // within its cooldown), its weight, and a running request count.
    Q_PROPERTY(QString lastSound READ lastSound NOTIFY played)
    Q_PROPERTY(double lastWeight READ lastWeight NOTIFY played)
    Q_PROPERTY(int playCount READ playCount NOTIFY played)
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
    static constexpr int thunkSteps = 6;
    // The 16-bit mono 44.1 kHz PCM a cue plays (thunk is rendered for weight);
    // for tests and offline inspection.
    static QByteArray render(const QString &name, double weight = 1.0);

    QString lastSound() const { return m_lastSound; }
    double lastWeight() const { return m_lastWeight; }
    int playCount() const { return m_playCount; }

    // Returns true when the cue was accepted (enabled, known, not silenced, past cooldown).
    // weight (0–1] scales loudness; page-turn "thunk"s grow with distance.
    Q_INVOKABLE bool play(const QString &name, double weight = 1.0);

signals:
    void enabledChanged();
    void reduceMotionChanged();
    void played();

private:
    bool ensureOutput();
    QString m_settingsPath;
    bool m_enabled = true;
    bool m_reduceMotion = false;
    bool m_silenced = false;
    bool m_audioOutput = true;
    bool m_outputFailed = false;
    QString m_lastSound;
    double m_lastWeight = 0;
    int m_playCount = 0;
    QHash<QString, QByteArray> m_tones;
    QList<QByteArray> m_thunks; // thunkSteps renders, lightest first
    QHash<QString, QElapsedTimer> m_lastPlayed;
    QAudioSink *m_sink = nullptr;
    QBuffer *m_buffer = nullptr;
};
