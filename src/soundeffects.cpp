#include "soundeffects.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QSettings>
#include <QtMath>
#include <memory>

namespace {
constexpr int sampleRate = 44100;
const char *settingsKey = "sound/effectsEnabled";
const char *motionKey = "motion/reduced";

struct Note { double startHz, endHz, ms, gain; };

// Sine sweep(s) with a soft attack and exponential decay, 16-bit mono PCM.
QByteArray synthesize(std::initializer_list<Note> notes)
{
    QByteArray pcm;
    for (const Note &note : notes) {
        const int samples = int(sampleRate * note.ms / 1000.0);
        const int attack = qMax(1, sampleRate / 500); // 2 ms
        double phase = 0;
        for (int i = 0; i < samples; ++i) {
            const double t = double(i) / samples;
            const double hz = note.startHz + (note.endHz - note.startHz) * t;
            phase += 2 * M_PI * hz / sampleRate;
            const double envelope = qMin(1.0, double(i) / attack) * qExp(-4.5 * t) * (1.0 - t);
            const auto sample = qint16(qBound(-1.0, qSin(phase) * envelope * note.gain, 1.0) * 32767);
            pcm.append(reinterpret_cast<const char *>(&sample), sizeof sample);
        }
    }
    return pcm;
}

QHash<QString, QByteArray> baseTones()
{
    QHash<QString, QByteArray> tones;
    tones.insert(QStringLiteral("tick"), synthesize({{1900, 1700, 18, 0.10}}));
    tones.insert(QStringLiteral("boundary"), synthesize({{420, 380, 60, 0.12}}));
    tones.insert(QStringLiteral("chime"), synthesize({{880, 880, 70, 0.09}, {1320, 1320, 110, 0.08}}));
    tones.insert(QStringLiteral("remove"), synthesize({{740, 520, 110, 0.10}}));
    tones.insert(QStringLiteral("error"), synthesize({{170, 110, 90, 0.20}}));
    return tones;
}

QSettings *openSettings(const QString &path)
{
    return path.isEmpty() ? new QSettings : new QSettings(path, QSettings::IniFormat);
}
}

SoundEffects::SoundEffects(QObject *parent, const QString &settingsPath, bool audioOutput)
    : QObject(parent), m_settingsPath(settingsPath), m_audioOutput(audioOutput)
{
    const QByteArray env = qgetenv("OMAWORD_SOUND").trimmed().toLower();
    m_silenced = env == "0" || env == "off" || env == "false";
    // Headless sessions (tests, CI) never open an audio device.
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) m_audioOutput = false;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    m_enabled = settings->value(settingsKey, true).toBool();
    m_reduceMotion = settings->value(motionKey, false).toBool();
    m_tones = baseTones();
}

SoundEffects::~SoundEffects()
{
    if (m_sink) m_sink->stop();
}

QByteArray SoundEffects::render(const QString &name)
{
    return baseTones().value(name);
}

QStringList SoundEffects::names()
{
    return {QStringLiteral("tick"), QStringLiteral("boundary"), QStringLiteral("chime"), QStringLiteral("remove"),
            QStringLiteral("error")};
}

void SoundEffects::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    settings->setValue(settingsKey, enabled);
    settings->sync();
    if (!enabled && m_sink) m_sink->stop();
    emit enabledChanged();
}

void SoundEffects::setReduceMotion(bool reduce)
{
    if (m_reduceMotion == reduce) return;
    m_reduceMotion = reduce;
    std::unique_ptr<QSettings> settings(openSettings(m_settingsPath));
    settings->setValue(motionKey, reduce);
    settings->sync();
    emit reduceMotionChanged();
}

bool SoundEffects::ensureOutput()
{
    if (m_sink) return true;
    if (m_outputFailed || !m_audioOutput) return false;
    const auto device = QMediaDevices::defaultAudioOutput();
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    if (device.isNull() || !device.isFormatSupported(format)) { m_outputFailed = true; return false; }
    m_sink = new QAudioSink(device, format, this);
    m_sink->setVolume(0.6);
    m_buffer = new QBuffer(this);
    return true;
}

bool SoundEffects::play(const QString &name)
{
    if (!m_enabled) return false;
    const auto tone = m_tones.constFind(name);
    if (tone == m_tones.cend()) return false;
    m_lastSound = name; ++m_playCount;
    emit played();
    if (m_silenced) return false;
    QElapsedTimer &last = m_lastPlayed[name];
    if (last.isValid() && last.elapsed() < cooldownMs) return false;
    last.start();
    if (!ensureOutput()) return true;
    // A new cue replaces any cue still sounding; QAudioSink pulls asynchronously.
    m_sink->stop();
    m_buffer->close();
    m_buffer->setData(*tone);
    m_buffer->open(QIODevice::ReadOnly);
    m_sink->start(m_buffer);
    if (m_sink->error() != QAudio::NoError) { m_sink->stop(); m_outputFailed = true; delete m_sink; m_sink = nullptr; }
    return true;
}
