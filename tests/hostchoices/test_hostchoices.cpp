#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QGuiApplication>
#include "streamingpreferences.h"

class FakeManager : public QObject {
    Q_OBJECT
public:
    int sequence = 0;
    Q_INVOKABLE int probeHostPlatform(QString address) { emit requested(address); return ++sequence; }
signals:
    void requested(QString address);
    void hostPlatformDetected(int requestId, QString address, int platform);
};
class FakePreferences : public QObject {
    Q_OBJECT
public:
    enum Capture {
        PLANK_CAPTURE_NVFBC_8BIT = StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT,
        PLANK_CAPTURE_X11_NATIVE10 = StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10,
        PLANK_CAPTURE_SCREENCAPTUREKIT = StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT
    };
    Q_ENUM(Capture)
};
class HostChoicesTest : public QObject {
    Q_OBJECT
private slots:
    void filtersAndIgnoresStaleReplies() {
        FakeManager manager;
        qmlRegisterSingletonInstance("ComputerManager", 1, 0, "ComputerManager", &manager);
        qmlRegisterUncreatableType<FakePreferences>("StreamingPreferences", 1, 0, "StreamingPreferences", "enums only");
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromUtf8(qgetenv("PLANK_CLIENT_SOURCE")) + "/app/gui/PlankCaptureSourceBox.qml"));
        QScopedPointer<QObject> box(component.create());
        QVERIFY2(box, qPrintable(component.errorString()));
        QCOMPARE(box->property("count").toInt(), 3);
        box->setProperty("hostAddress", "mac.test");
        box->setProperty("probingEnabled", true);
        QTRY_COMPARE(manager.sequence, 1);
        emit manager.hostPlatformDetected(1, "mac.test", 2);
        QCOMPARE(box->property("count").toInt(), 1);
        QCOMPARE(box->property("captureSource").toInt(), 2);
        box->setProperty("hostAddress", "linux.test");
        QCOMPARE(box->property("count").toInt(), 3);
        emit manager.hostPlatformDetected(1, "mac.test", 2);
        QCOMPARE(box->property("count").toInt(), 3);
        QTRY_COMPARE(manager.sequence, 2);
        emit manager.hostPlatformDetected(2, "linux.test", 1);
        QCOMPARE(box->property("count").toInt(), 2);
        QCOMPARE(box->property("captureSource").toInt(), 0);
        box->setProperty("currentIndex", 1);
        QCOMPARE(box->property("captureSource").toInt(), 1);
        box->setProperty("hostAddress", "offline.test");
        QTRY_COMPARE(manager.sequence, 3);
        emit manager.hostPlatformDetected(3, "offline.test", 0);
        QCOMPARE(box->property("count").toInt(), 3);
        QCOMPARE(box->property("captureSource").toInt(), 1);
        box->setProperty("probingEnabled", false);
        emit manager.hostPlatformDetected(3, "offline.test", 2);
        QCOMPARE(box->property("count").toInt(), 3);
    }
};
QTEST_MAIN(HostChoicesTest)
#include "test_hostchoices.moc"
