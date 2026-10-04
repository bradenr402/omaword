#include <QtTest/QTest>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include "soundeffects.h"

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
