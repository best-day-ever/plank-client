#include <QtTest>
#include <QProcess>
#include <QTimer>
#include "../../app/macapplication.h"

// Each case gets a real application lifetime. Reusing an application after it
// has committed to exit would hide precisely the state we need to exercise.
static int runScenario(MacApplication& application, const QString& scenario)
{
    int failures = 0;
    int requests = 0;
    int completions = 0;
    int quits = 0;
    const bool idle = scenario == "idle";
    const bool disconnect = scenario == "disconnect";
    const bool startupFailure = scenario == "startup-failure";
    const int sessions = scenario == "overlapping-cleanup" ? 2 : (idle ? 0 : 1);
    const auto check = [&](bool result) {
        if (!result) {
            ++failures;
            qWarning() << "Failed lifecycle assertion in" << scenario;
        }
    };
    const auto requestQuit = [&] {
        QEvent event(QEvent::Quit);
        QCoreApplication::sendEvent(&application, &event);
    };
    QObject::connect(&application, &MacApplication::exitRequested, &application, [&] {
        ++requests;
        check(quits == 0);
        check(!application.beginSession());
        // Session cleanup is asynchronous. Repeated native Quit must neither
        // duplicate cancellation nor terminate before completion.
        QTimer::singleShot(10, &application, [&] {
            check(quits == 0);
            ++completions;
            application.endSession();
            if (sessions == 2) {
                QTimer::singleShot(10, &application, [&] {
                    check(quits == 0);
                    ++completions;
                    application.endSession();
                });
            }
        });
    });
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application, [&] {
        ++quits;
        check(completions == sessions);
    });
    QTimer::singleShot(0, &application, [&] {
        for (int i = 0; i < sessions; ++i) {
            check(application.beginSession());
        }
        if (disconnect || startupFailure) {
            ++completions;
            application.endSession();
            // Completing a session alone must keep the application open.
            QTimer::singleShot(20, &application, [&] {
                check(quits == 0 && requests == 0);
                requestQuit();
            });
        }
        else {
            requestQuit();
            if (!idle) {
                check(quits == 0 && requests == 1);
            }
            if (scenario == "repeated-quit") {
                requestQuit();
                requestQuit();
                check(requests == 1);
            }
        }
    });
    QTimer::singleShot(3000, &application, [&] {
        ++failures;
        application.exit(1); // Watchdog, not the production Quit path.
    });
    application.exec();
    check(quits == 1);
    check(requests == ((idle || disconnect || startupFailure) ? 0 : 1));
    return failures == 0 ? 0 : 1;
}

class TestMacApplication : public QObject
{
    Q_OBJECT
private slots:
    void lifecycle_data()
    {
        QTest::addColumn<QString>("scenario");
        for (const char* name : {"idle", "active", "repeated-quit", "disconnect",
                                 "startup-failure", "overlapping-cleanup"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }
    void lifecycle()
    {
        QFETCH(QString, scenario);
        QProcess process;
        process.start(QCoreApplication::applicationFilePath(), {"--scenario", scenario});
        QVERIFY(process.waitForFinished(10000));
        const QByteArray output = process.readAllStandardError() + process.readAllStandardOutput();
        QVERIFY2(process.exitStatus() == QProcess::NormalExit, output.constData());
        QVERIFY2(process.exitCode() == 0, output.constData());
    }
};

int main(int argc, char** argv)
{
    MacApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--scenario") {
        return runScenario(application, QString::fromLocal8Bit(argv[2]));
    }
    TestMacApplication test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_macapplication.moc"
