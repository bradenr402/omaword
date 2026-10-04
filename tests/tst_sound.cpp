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
    void testHook() {
        SoundEffects sounds(nullptr, path(), false);
        QCOMPARE(SoundEffects::names(), QStringList({"tick", "boundary", "chime", "remove", "error"}));
        QSignalSpy played(&sounds, &SoundEffects::played);
        QVERIFY(sounds.play("chime"));
        QCOMPARE(sounds.lastSound(), QString("chime"));
        // Requests inside the cooldown are still recorded.
        QVERIFY(!sounds.play("chime"));
        QCOMPARE(sounds.playCount(), 2); QCOMPARE(played.count(), 2);
        // Unknown cues and the sound toggle record nothing.
        QVERIFY(!sounds.play("nonexistent")); QCOMPARE(sounds.lastSound(), QString("chime"));
        sounds.setEnabled(false);
        QVERIFY(!sounds.play("tick")); QCOMPARE(sounds.playCount(), 2);
        // Silenced runs (bin/test) still record what would have played.
        qputenv("OMAWORD_SOUND", "0");
        SoundEffects silenced(nullptr, dir.filePath("other.ini"), false);
        QVERIFY(!silenced.play("tick")); QCOMPARE(silenced.lastSound(), QString("tick"));
    }
    void buffersAreClean() {
        for (const auto &name : SoundEffects::names()) {
            const QList<double> x = samples(SoundEffects::render(name));
            QVERIFY2(x.size() > 200, qPrintable(name));
            double peak = 0;
            for (double v : x) peak = qMax(peak, std::abs(v));
            QVERIFY2(peak < 0.99 && peak > 0.02, qPrintable(name));
        }
        QVERIFY(SoundEffects::render("nonexistent").isEmpty());
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
