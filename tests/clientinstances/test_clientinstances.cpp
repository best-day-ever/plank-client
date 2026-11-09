#include "../../app/backend/clientinstances.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QTimer>

class Worker : public QProcess
{
public:
    ~Worker() { if (state() != NotRunning) { kill(); waitForFinished(); } }
    bool startRegistry(const QString& directory)
    {
        start(QCoreApplication::applicationFilePath(), {"--registry-worker", directory});
        if (!waitForStarted() || !waitForReadyRead()) return false;
        return readAllStandardOutput().trimmed() == "ready";
    }
};

class TestClientInstances : public QObject
{
    Q_OBJECT
private slots:
    void independentWindowsBlockUpdate()
    {
        QTemporaryDir temp;
        ClientInstances parent(temp.path());
        QVERIFY(parent.registerInstance());
        QVERIFY(!parent.hasOtherInstances());
        Worker first, second;
        QVERIFY(first.startRegistry(temp.path()));
        QVERIFY(second.startRegistry(temp.path()));
        QVERIFY(parent.hasOtherInstances());
        QVERIFY(!parent.beginUpdate());
        first.kill(); QVERIFY(first.waitForFinished());
        QVERIFY(parent.hasOtherInstances());
        QVERIFY(!parent.beginUpdate());
        second.kill(); QVERIFY(second.waitForFinished());
        // Killed workers leave stale lock files, which must not block updates.
        QVERIFY(!parent.hasOtherInstances());
        QVERIFY(parent.beginUpdate());
    }

    void updateSerializesStartupAndAnotherUpdater()
    {
        QTemporaryDir temp;
        ClientInstances parent(temp.path());
        QVERIFY(parent.registerInstance());
        QVERIFY(parent.beginUpdate());
        QVERIFY(parent.beginUpdate());
        ClientInstances newcomer(temp.path());
        QVERIFY(!newcomer.registerInstance());
        Worker blocked;
        QVERIFY(!blocked.startRegistry(temp.path()));
        QVERIFY(blocked.waitForFinished() || blocked.state() == QProcess::NotRunning);
        QCOMPARE(blocked.exitCode(), 3);
        QVERIFY(!parent.openNewConnection());
        parent.endUpdate();
        QVERIFY(newcomer.registerInstance());
        QVERIFY(!newcomer.beginUpdate());
        QVERIFY(!parent.beginUpdate());
    }

    void normalCloseReleasesInstance()
    {
        QTemporaryDir temp;
        ClientInstances parent(temp.path());
        QVERIFY(parent.registerInstance());
        {
            ClientInstances other(temp.path());
            QVERIFY(other.registerInstance());
            QVERIFY(parent.hasOtherInstances());
        }
        QVERIFY(!parent.hasOtherInstances());
        QVERIFY(parent.beginUpdate());
        parent.endUpdate();
        QVERIFY(parent.beginUpdate());
    }

    void unusableRegistryFailsClosed()
    {
        QTemporaryDir temp;
        QFile file(temp.filePath("file"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        ClientInstances broken(file.fileName());
        QVERIFY(!broken.registerInstance());
        QVERIFY(broken.hasOtherInstances());
        QVERIFY(!broken.beginUpdate());
    }

    void launchesIndependentWindowWithoutArguments()
    {
        QTemporaryDir temp;
        ClientInstances parent(temp.path());
        QVERIFY(parent.registerInstance());
        qputenv("PLANK_TEST_NEW_WINDOW", temp.path().toUtf8());
        const bool started = parent.openNewConnection();
        qunsetenv("PLANK_TEST_NEW_WINDOW");
        QVERIFY(started);
        QTRY_VERIFY(QFile::exists(temp.filePath("child-ready")));
        QVERIFY(parent.hasOtherInstances());
        QVERIFY(!parent.beginUpdate());
        QFile stop(temp.filePath("stop"));
        QVERIFY(stop.open(QIODevice::WriteOnly));
        stop.close();
        QTRY_VERIFY(!parent.hasOtherInstances());
        QVERIFY(parent.beginUpdate());
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    const QString detachedDirectory = qEnvironmentVariable("PLANK_TEST_NEW_WINDOW");
    if (args.value(1) == "--registry-worker" || !detachedDirectory.isEmpty()) {
        const QString directory = detachedDirectory.isEmpty() ? args.value(2) : detachedDirectory;
        ClientInstances instance(directory);
        if (!instance.registerInstance()) return 3;
        if (detachedDirectory.isEmpty()) {
            fwrite("ready\n", 1, 6, stdout); fflush(stdout);
        } else {
            if (args.size() != 1) return 4;
            QFile ready(QDir(directory).filePath("child-ready"));
            if (!ready.open(QIODevice::WriteOnly)) return 5;
            ready.close();
        }
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
            if (QFile::exists(QDir(directory).filePath("stop"))) app.quit();
        });
        poll.start(20);
        QTimer::singleShot(10000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    TestClientInstances test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_clientinstances.moc"
