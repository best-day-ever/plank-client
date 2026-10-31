#include "../../app/backend/internetconnectivity.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>

using Monitor = InternetConnectivity;
using Settings = PlankClientPolicy::InternetCheck;

class ConnectivityTest : public QObject
{
    Q_OBJECT
private:
    static Monitor::PingCommand stub(int code, int delayMs = 0, const QString& error = QString())
    {
        return {QCoreApplication::applicationFilePath(),
                {QStringLiteral("--ping-stub"), QString::number(code), QString::number(delayMs), error}, 1};
    }
private slots:
    void policyDefaultsAndExplicitValues()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("client.conf");
        auto config = PlankClientPolicy(path).internetCheck();
        QVERIFY(config.enabled);
        QCOMPARE(config.hosts, QStringList({"8.8.8.8", "1.1.1.1", "9.9.9.9"}));
        QCOMPARE(config.intervalMs, 15000);
        QCOMPARE(config.timeoutMs, 1000);
        QVERIFY(config.error.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[network]\ninternet_check_hosts = 192.0.2.1, 2001:db8::1\n"
                   "internet_check_interval_seconds = 30\ninternet_check_timeout_ms = 700\n");
        file.close();
        config = PlankClientPolicy(path).internetCheck();
        QCOMPARE(config.hosts, QStringList({"192.0.2.1", "2001:db8::1"}));
        QCOMPARE(config.intervalMs, 30000);
        QCOMPARE(config.timeoutMs, 700);
        QVERIFY(config.error.isEmpty());
    }

    void invalidPolicy_data()
    {
        QTest::addColumn<QByteArray>("line");
        QTest::newRow("empty targets") << QByteArray("internet_check_hosts =");
        QTest::newRow("option injection") << QByteArray("internet_check_hosts = -f");
        QTest::newRow("shell injection") << QByteArray("internet_check_hosts = 192.0.2.1;exit 0");
        QTest::newRow("DNS name") << QByteArray("internet_check_hosts = example.com");
        QTest::newRow("multicast") << QByteArray("internet_check_hosts = ff02::1");
        QTest::newRow("unspecified") << QByteArray("internet_check_hosts = 0.0.0.0");
        QTest::newRow("broadcast") << QByteArray("internet_check_hosts = 255.255.255.255");
        QTest::newRow("scoped") << QByteArray("internet_check_hosts = fe80::1%eth0");
        QTest::newRow("empty entry") << QByteArray("internet_check_hosts = 192.0.2.1,,192.0.2.2");
        QTest::newRow("too many") << QByteArray("internet_check_hosts = 192.0.2.1,192.0.2.2,192.0.2.3,192.0.2.4,192.0.2.5,192.0.2.6,192.0.2.7,192.0.2.8,192.0.2.9");
        QTest::newRow("short interval") << QByteArray("internet_check_interval_seconds = 0");
        QTest::newRow("large interval") << QByteArray("internet_check_interval_seconds = 3601");
        QTest::newRow("overflow interval") << QByteArray("internet_check_interval_seconds = 4294967296");
        QTest::newRow("short timeout") << QByteArray("internet_check_timeout_ms = 99");
        QTest::newRow("long timeout") << QByteArray("internet_check_timeout_ms = 10001");
        QTest::newRow("invalid timeout") << QByteArray("internet_check_timeout_ms = forever");
    }

    void invalidPolicy()
    {
        QFETCH(QByteArray, line);
        QTemporaryDir dir;
        QFile file(dir.filePath("client.conf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[network]\n" + line + "\n");
        file.close();
        int attempts = 0;
        Monitor monitor(PlankClientPolicy(file.fileName()).internetCheck(), nullptr,
                        [&attempts](const QString&, int) { ++attempts; return stub(0); });
        monitor.start();
        monitor.checkNow();
        QCOMPARE(monitor.state(), Monitor::Unavailable);
        QCOMPARE(attempts, 0); // Never fall back to public IPs after a config error.
    }

    void disabledMakesNoProbes()
    {
        Settings settings;
        settings.enabled = false;
        int attempts = 0;
        Monitor monitor(settings, nullptr, [&attempts](const QString&, int) { ++attempts; return stub(0); });
        monitor.start();
        monitor.checkNow();
        monitor.networkChanged();
        monitor.refreshIfStale();
        QTest::qWait(300);
        QCOMPARE(monitor.state(), Monitor::Disabled);
        QCOMPARE(attempts, 0);
    }

    void orderedFallbackAndPeriodicRestart()
    {
        Settings settings;
        settings.intervalMs = 1100;
        QStringList attempted;
        Monitor monitor(settings, nullptr, [&attempted](const QString& host, int timeout) {
            attempted.append(host);
            Q_ASSERT(timeout == 1000);
            return stub(host == "1.1.1.1" ? 0 : 1);
        });
        QCOMPARE(monitor.state(), Monitor::Checking);
        monitor.start();
        QTRY_COMPARE(monitor.state(), Monitor::Reachable);
        QCOMPARE(attempted, QStringList({"8.8.8.8", "1.1.1.1"}));
        QSignalSpy updates(&monitor, &Monitor::stateChanged);
        QTRY_COMPARE_WITH_TIMEOUT(attempted.size(), 4, 3000);
        QTest::qWait(50);
        QCOMPARE(attempted, QStringList({"8.8.8.8", "1.1.1.1", "8.8.8.8", "1.1.1.1"}));
        QCOMPARE(monitor.state(), Monitor::Reachable);
        QCOMPARE(updates.size(), 0); // No Checking flicker between periodic rounds.
        monitor.stop();
    }

    void noResponseRecoversWithoutInteraction()
    {
        Settings settings;
        bool online = false;
        int attempts = 0;
        Monitor monitor(settings, nullptr, [&online, &attempts](const QString&, int) {
            ++attempts;
            return stub(online ? 0 : 1);
        });
        monitor.start();
        QTRY_COMPARE(monitor.state(), Monitor::NoResponse);
        QCOMPARE(attempts, 3);
        online = true;
        monitor.networkChanged();
        QTRY_COMPARE(monitor.state(), Monitor::Reachable);
        QCOMPARE(attempts, 4);
        monitor.refreshIfStale();
        QCOMPARE(attempts, 4); // Focus changes cannot produce a ping storm.
    }

    void deadlineDoesNotBlockUiOrLeaveChildren()
    {
        Settings settings;
        settings.timeoutMs = 100;
        int heartbeat = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&heartbeat] { ++heartbeat; });
        timer.start(10);
        Monitor monitor(settings, nullptr, [](const QString&, int) { return stub(0, 60000); });
        QElapsedTimer elapsed;
        elapsed.start();
        monitor.start();
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), Monitor::NoResponse, 2500);
        QVERIFY(elapsed.elapsed() < 2500);
        QVERIFY(heartbeat >= 5);
        QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    }

    void unavailableIsNotAnInternetOutage()
    {
        Settings settings;
        settings.hosts = {"192.0.2.1"};
        Monitor monitor(settings, nullptr, [](const QString&, int) {
            return Monitor::PingCommand{QStringLiteral("/no-such-plank-test-ping"), {}, 1};
        });
        monitor.start();
        QTRY_COMPARE(monitor.state(), Monitor::Unavailable);
        QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());
    }

    void probeErrorCanFallBack()
    {
        Settings settings;
        int attempts = 0;
        Monitor monitor(settings, nullptr, [&attempts](const QString&, int) {
            return stub(++attempts == 1 ? 64 : 0);
        });
        monitor.start();
        QTRY_COMPARE(monitor.state(), Monitor::Reachable);
        QCOMPARE(attempts, 2);
    }

    void networkErrorIsNotUtilityFailure_data()
    {
        QTest::addColumn<QString>("error");
        QTest::addColumn<int>("expectedState");
        QTest::newRow("no route") << QStringLiteral("ping: connect: Network is unreachable") << int(Monitor::NoResponse);
        QTest::newRow("host unreachable") << QStringLiteral("ping: sendto: No route to host") << int(Monitor::NoResponse);
        QTest::newRow("permission denied") << QStringLiteral("ping: socket: Operation not permitted") << int(Monitor::Unavailable);
    }

    void networkErrorIsNotUtilityFailure()
    {
        QFETCH(QString, error);
        QFETCH(int, expectedState);
        Settings settings;
        settings.hosts = {"192.0.2.1"};
        Monitor monitor(settings, nullptr, [error](const QString&, int) { return stub(64, 0, error); });
        monitor.start();
        QTRY_COMPARE(int(monitor.state()), expectedState);
    }

    void restartAndStopDiscardCancelledResults()
    {
        Settings settings;
        settings.hosts = {"192.0.2.1"};
        settings.timeoutMs = 1000;
        int attempts = 0;
        Monitor monitor(settings, nullptr, [&attempts](const QString&, int) {
            return ++attempts == 1 ? stub(0, 500) : stub(1);
        });
        QSignalSpy updates(&monitor, &Monitor::stateChanged);
        monitor.start();
        QTest::qWait(50);
        monitor.checkNow();
        monitor.checkNow(); // Coalesces while cancellation is still in progress.
        QTRY_COMPARE(monitor.state(), Monitor::NoResponse);
        QCOMPARE(attempts, 2);
        QCOMPARE(updates.size(), 1);
        QTest::qWait(550);
        QCOMPARE(monitor.state(), Monitor::NoResponse);
        monitor.stop();
        monitor.checkNow();
        QCOMPARE(attempts, 2);
        QTRY_VERIFY(monitor.findChildren<QProcess*>().isEmpty());

        Monitor stopped(settings, nullptr, [](const QString&, int) { return stub(0, 60000); });
        QSignalSpy stoppedUpdates(&stopped, &Monitor::stateChanged);
        stopped.start();
        QTest::qWait(50);
        stopped.stop();
        QTRY_VERIFY(stopped.findChildren<QProcess*>().isEmpty());
        QCOMPARE(stoppedUpdates.size(), 0);
    }

    void systemPingLoopback_data()
    {
        QTest::addColumn<QString>("address");
        QTest::newRow("IPv4") << QStringLiteral("127.0.0.1");
        QTest::newRow("IPv6") << QStringLiteral("::1");
    }

    void systemPingLoopback()
    {
        QFETCH(QString, address);
        Settings settings;
        settings.hosts = {address};
        settings.timeoutMs = 2000;
        // Use the actual OS utility as an ordinary runner account. No public
        // Internet dependency and no elevated process or new PLANK capability.
        Monitor monitor(settings);
        monitor.start();
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), Monitor::Reachable, 4000);
    }

    void statusUi()
    {
        qmlRegisterUncreatableType<Monitor>("InternetConnectivity", 1, 0, "InternetConnectivity", "test");
        Settings settings;
        settings.hosts = {"192.0.2.1"};
        bool online = false;
        Monitor monitor(settings, nullptr, [&online](const QString&, int) { return stub(online ? 0 : 1); });
        QQmlEngine engine;
        QSignalSpy warnings(&engine, &QQmlEngine::warnings);
        engine.rootContext()->setContextProperty("testMonitor", &monitor);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick 2.15
            import QtQuick.Controls 2.15
            import QtQuick.Controls.Material 2.15
            import QtQuick.Layouts 1.15
            import "qrc:/gui"
            ApplicationWindow {
                width: 1000; height: 160; visible: true
                Material.theme: Material.Dark
                Material.background: "#2D3035"
                header: ToolBar {
                    height: 56
                    background: Rectangle { color: "#393D43" }
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                        Label { text: "1.2.10-client-connectivity"; font.pointSize: 9; color: "#B7BDC5" }
                        Label { text: "Computers"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
                        PlankInternetStatus { monitor: testMonitor }
                        ToolButton { text: "+" }
                        ToolButton { text: "Settings" }
                    }
                }
            }
        )", QUrl("qrc:/connectivity-test.qml"));
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto* window = qobject_cast<QQuickWindow*>(root.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto* status = root->findChild<QQuickItem*>("internetStatus");
        auto* label = root->findChild<QQuickItem*>("internetStatusLabel");
        QVERIFY(status && label);
        QCOMPARE(label->property("text").toString(), QString("Internet: Checking"));
        QVERIFY(status->width() > 80);
        QVERIFY(status->height() <= 56);
        monitor.start();
        QTRY_COMPARE(label->property("text").toString(), QString("Internet: No response"));
        window->resize(720, 160);
        QTest::qWait(50);
        QVERIFY(status->mapToScene(QPointF()).x() >= 0);
        QVERIFY(status->mapToScene(QPointF(status->width(), 0)).x() <= window->width());
        QVERIFY(label->property("contentWidth").toReal() <= label->width() + 1);
        const QString captureDirectory = qEnvironmentVariable("PLANK_CONNECTIVITY_CAPTURE_DIR");
        if (!captureDirectory.isEmpty()) {
            QVERIFY(QDir().mkpath(captureDirectory));
            QVERIFY(window->grabWindow().save(captureDirectory + "/internet-no-response.png"));
        }
        online = true;
        monitor.checkNow();
        QTRY_COMPARE(label->property("text").toString(), QString("Internet: Reachable"));
        if (!captureDirectory.isEmpty())
            QVERIFY(window->grabWindow().save(captureDirectory + "/internet-reachable.png"));
        settings.enabled = false;
        Monitor disabled(settings);
        status->setProperty("monitor", QVariant::fromValue(static_cast<QObject*>(&disabled)));
        QTRY_VERIFY(!status->isVisible());
        root.reset();
        QCOMPARE(warnings.size(), 0);
    }
};

int main(int argc, char** argv)
{
    // Deterministic child processes exercise the real QProcess/deadline/reaping
    // path. This mode exists only in this test binary, never the shipped Client.
    if (argc == 5 && QByteArray(argv[1]) == "--ping-stub") {
        QCoreApplication app(argc, argv);
        QFile error;
        if (error.open(stderr, QIODevice::WriteOnly)) {
            error.write(argv[4]);
            error.flush();
        }
        const int code = QByteArray(argv[2]).toInt();
        QTimer::singleShot(QByteArray(argv[3]).toInt(), &app, [code] { QCoreApplication::exit(code); });
        return app.exec();
    }
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Material");
    ConnectivityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_internetconnectivity.moc"
