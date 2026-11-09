#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <memory>

class PollingManager : public QObject {
    Q_OBJECT
public:
    int starts = 0, stops = 0;
    Q_INVOKABLE void startPolling() { ++starts; }
    Q_INVOKABLE void stopPollingAsync() { ++stops; }
};

class TestHostPolling : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<QObject> create(QQmlEngine& engine, PollingManager& manager,
                                   bool visible, bool active, int interval = 300000) {
        QQmlComponent component(&engine, QUrl("qrc:/hostpolling/PlankHostPolling.qml"));
        auto object = std::unique_ptr<QObject>(component.createWithInitialProperties({
            {"manager", QVariant::fromValue(&manager)}, {"windowVisible", visible},
            {"windowActive", active}, {"inactivityInterval", interval}}));
        if (!object) qWarning() << component.errors();
        return object;
    }
private slots:
    void visibleInactiveStartsWithoutActivation() {
        QQmlEngine engine; PollingManager manager;
        auto object = create(engine, manager, true, false);
        QVERIFY(object); QCOMPARE(manager.starts, 1); QCOMPARE(manager.stops, 0);
        QVERIFY(object->property("pollingActive").toBool());
        object->setProperty("windowActive", true);
        QCOMPARE(manager.starts, 1); // Activation must not add another reference.
        object->setProperty("windowVisible", false);
        QCOMPARE(manager.stops, 1);
        object->setProperty("windowVisible", true);
        QCOMPARE(manager.starts, 2);
        object.reset(); QCOMPARE(manager.stops, 2);
    }
    void hiddenStartupWaitsUntilShown() {
        QQmlEngine engine; PollingManager manager;
        auto object = create(engine, manager, false, false);
        QVERIFY(object); QCOMPARE(manager.starts, 0);
        object->setProperty("windowActive", true);
        QCOMPARE(manager.starts, 0);
        object->setProperty("windowVisible", true);
        QCOMPARE(manager.starts, 1);
    }
    void inactiveTimeoutAndFocusResume() {
        QQmlEngine engine; PollingManager manager;
        auto object = create(engine, manager, true, false, 20);
        QVERIFY(object); QCOMPARE(manager.starts, 1);
        QTRY_COMPARE_WITH_TIMEOUT(manager.stops, 1, 1000);
        QVERIFY(!object->property("pollingActive").toBool());
        object->setProperty("windowActive", true);
        QCOMPARE(manager.starts, 2);
        QTest::qWait(50); QCOMPARE(manager.stops, 1);
        object->setProperty("windowActive", false);
        object->setProperty("windowVisible", false);
        QCOMPARE(manager.stops, 2);
        QTest::qWait(50); QCOMPARE(manager.stops, 2);
    }
};
QTEST_MAIN(TestHostPolling)
#include "test_hostpolling.moc"
