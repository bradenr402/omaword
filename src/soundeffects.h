#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <memory>

// Short, quiet UI cues synthesized in memory and played through PipeWire. One
// instance is shared by every window of the process; failures (no PipeWire,
// disabled env) stay silent and are retried later.
class SoundEffects : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    // App-wide "Reduce motion" preference (QSettings motion/reduced), shared by
    // every window with the sound preference; it lives here so one object owns
    // all persisted feedback preferences.
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
    // Test hook: the last cue requested while enabled (even when silenced or
    // within its cooldown) and a running request count.
    Q_PROPERTY(QString lastSound READ lastSound NOTIFY played)
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
    // error and boundary sound once per burst (a held key), not per repeat.
    static constexpr int burstCooldownMs = 500;
    static int cooldownFor(const QString &name);
    // The 16-bit mono 44.1 kHz PCM a cue plays (before the output volume);
    // for tests and offline inspection.
    static QByteArray render(const QString &name);

    QString lastSound() const { return m_lastSound; }
    int playCount() const { return m_playCount; }

    // Returns true when the cue was accepted (enabled, known, not silenced, past cooldown).
    Q_INVOKABLE bool play(const QString &name);

signals:
    void enabledChanged();
    void reduceMotionChanged();
    void played();
    // The first cue this user ever heard (persisted): say how to turn them off.
    void introduced();

private:
    struct Output;
    static constexpr int retryMs = 5000;
    bool ensureOutput();
    QString m_settingsPath;
    bool m_enabled = true;
    bool m_reduceMotion = false;
    bool m_silenced = false;
    bool m_audioOutput = true;
    bool m_introduced = false;
    QString m_lastSound;
    int m_playCount = 0;
    QHash<QString, QByteArray> m_tones;
    QHash<QString, QElapsedTimer> m_lastPlayed;
    std::unique_ptr<Output> m_output;
    QElapsedTimer m_outputAttempt;
};
