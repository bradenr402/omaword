#include <QtTest/QTest>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "soundeffects.h"
#include <cmath>

namespace {
QList<double> samples(const QByteArray &pcm)
{
    QList<double> out;
    const auto *in = reinterpret_cast<const qint16 *>(pcm.constData());
    for (qsizetype i = 0; i < pcm.size() / 2; ++i) out.append(in[i] / 32768.0);
    return out;
}
double rms(const QList<double> &x, qsizetype from, qsizetype count)
{
    double sum = 0;
    for (qsizetype i = from; i < from + count; ++i) sum += x[i] * x[i];
    return std::sqrt(sum / count);
}
// Spectral flatness (geometric / arithmetic mean of power) of a Hann-windowed
// 1024-sample frame over 300 Hz–8 kHz: ~0 for a tone, larger for noise.
double flatness(const QList<double> &x, qsizetype from)
{
    const int n = 1024;
    double logSum = 0, sum = 0; int bins = 0;
    for (int k = 300 * n / 44100 + 1; k < 8000 * n / 44100; ++k) {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
            re += x[from + i] * w * std::cos(2 * M_PI * k * i / n);
            im -= x[from + i] * w * std::sin(2 * M_PI * k * i / n);
        }
        const double power = re * re + im * im + 1e-18;
        logSum += std::log(power); sum += power; ++bins;
    }
    return std::exp(logSum / bins) / (sum / bins);
}
}

class SoundTest : public QObject
{
    Q_OBJECT
    QTemporaryDir dir;
    QString path() const { return dir.filePath("sound.ini"); }
private slots:
    void init() { QFile::remove(path()); qunsetenv("OMAWORD_SOUND"); }
    void enabledByDefaultAndPersists() {
        {
            SoundEffects sounds(nullptr, path(), false);
            QVERIFY(sounds.enabled());
            QSignalSpy changed(&sounds, &SoundEffects::enabledChanged);
            sounds.setEnabled(false);
            sounds.setEnabled(false);
            QCOMPARE(changed.count(), 1);
            QCOMPARE(sounds.property("enabled").toBool(), false);
        }
        SoundEffects reopened(nullptr, path(), false);
        QVERIFY(!reopened.enabled());
        QVERIFY(reopened.setProperty("enabled", true));
        QVERIFY(SoundEffects(nullptr, path(), false).enabled());
    }
    void disabledAndSilencedAreNoOps() {
        SoundEffects sounds(nullptr, path(), false);
        sounds.setEnabled(false);
        for (const auto &name : SoundEffects::names()) QVERIFY(!sounds.play(name));
        sounds.setEnabled(true);
        QVERIFY(sounds.play("tick"));
        qputenv("OMAWORD_SOUND", "0");
        SoundEffects silenced(nullptr, path(), true);
        QVERIFY(silenced.silenced());
        QVERIFY(silenced.enabled());
        for (const auto &name : SoundEffects::names()) QVERIFY(!silenced.play(name));
    }
    void cooldownAndUnknownNames() {
        SoundEffects sounds(nullptr, path(), false);
        QVERIFY(!sounds.play("nonexistent"));
        QVERIFY(!sounds.play(QString()));
        QVERIFY(sounds.play("tick"));
        QVERIFY(!sounds.play("tick"));
        // Cooldown is per cue: other cues still play immediately.
        QVERIFY(sounds.play("boundary"));
        QVERIFY(sounds.play("chime"));
        QTest::qWait(SoundEffects::cooldownMs + 20);
        QVERIFY(sounds.play("tick"));
    }
    void pageSoundsAndTestHook() {
        SoundEffects sounds(nullptr, path(), false);
        QVERIFY(SoundEffects::names().contains("pageTurn"));
        QVERIFY(SoundEffects::names().contains("thunk"));
        QSignalSpy played(&sounds, &SoundEffects::played);
        QVERIFY(sounds.play("pageTurn"));
        QCOMPARE(sounds.lastSound(), QString("pageTurn")); QCOMPARE(sounds.lastWeight(), 1.0);
        QVERIFY(sounds.play("thunk", 0.4));
        QCOMPARE(sounds.lastSound(), QString("thunk")); QCOMPARE(sounds.lastWeight(), 0.4);
        // Weights clamp; requests inside the cooldown are still recorded.
        QVERIFY(!sounds.play("thunk", 7));
        QCOMPARE(sounds.lastWeight(), 1.0); QCOMPARE(sounds.playCount(), 3); QCOMPARE(played.count(), 3);
        QTest::qWait(SoundEffects::cooldownMs + 20);
        QVERIFY(sounds.play("thunk", 0)); QCOMPARE(sounds.lastWeight(), 0.05);
        // Unknown cues and the sound toggle record nothing.
        QVERIFY(!sounds.play("nonexistent")); QCOMPARE(sounds.lastSound(), QString("thunk"));
        sounds.setEnabled(false);
        QVERIFY(!sounds.play("pageTurn")); QCOMPARE(sounds.playCount(), 4);
        // Silenced runs (bin/test) still record what would have played.
        qputenv("OMAWORD_SOUND", "0");
        SoundEffects silenced(nullptr, dir.filePath("other.ini"), false);
        QVERIFY(!silenced.play("pageTurn")); QCOMPARE(silenced.lastSound(), QString("pageTurn"));
    }
    void buffersAreCleanAndPagesAreNoise() {
        for (const auto &name : SoundEffects::names()) {
            const QList<double> x = samples(SoundEffects::render(name));
            QVERIFY2(x.size() > 200, qPrintable(name));
            double peak = 0, mean = 0;
            for (double v : x) { peak = qMax(peak, std::abs(v)); mean += v; }
            QVERIFY2(peak < 0.99 && peak > 0.02, qPrintable(name));
            if (name == "pageTurn" || name == "thunk") {
                // Quiet, DC-free, and faded at both edges (no clicks).
                QVERIFY2(peak < 0.5, qPrintable(name));
                QVERIFY2(std::abs(mean / x.size()) < 1e-3, qPrintable(name));
                QVERIFY2(std::abs(x.first()) < 1e-3 && std::abs(x.last()) < 1e-3, qPrintable(name));
                QVERIFY2(rms(x, 0, 44) < 0.2 * peak && rms(x, x.size() - 44, 44) < 0.2 * peak, qPrintable(name));
            }
        }
        const QList<double> page = samples(SoundEffects::render("pageTurn"));
        const double ms = page.size() * 1000.0 / 44100;
        QVERIFY(ms >= 180 && ms <= 260);
        // Broadband: well above a pure tone's flatness at the start and middle.
        QVERIFY(flatness(page, 0) > 0.05);
        QVERIFY(flatness(page, page.size() / 2 - 512) > 0.05);
        QList<double> tone;
        for (int i = 0; i < 2048; ++i) tone.append(0.3 * std::sin(2 * M_PI * 1500 * i / 44100));
        QVERIFY(flatness(tone, 0) < 0.01);
        // Heavier thunks are longer and louder, but capped.
        const auto light = samples(SoundEffects::render("thunk", 0.1)), heavy = samples(SoundEffects::render("thunk", 1.0));
        QVERIFY(heavy.size() > light.size());
        QVERIFY(rms(heavy, 0, heavy.size()) > rms(light, 0, light.size()));
        QCOMPARE(SoundEffects::render("thunk", 50), SoundEffects::render("thunk", 1.0));
        QVERIFY(heavy.size() * 1000.0 / 44100 < 450);
    }
    void invokableFromMetaObject() {
        SoundEffects sounds(nullptr, path(), false);
        bool accepted = false;
        QVERIFY(QMetaObject::invokeMethod(&sounds, "play", Q_RETURN_ARG(bool, accepted), Q_ARG(QString, "remove")));
        QVERIFY(accepted);
    }
};

int main(int argc, char **argv) {
    // Never reach a real device even if the suite is run outside bin/test.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    SoundTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_sound.moc"
