#include <QtTest>
#include <QStandardPaths>
#include <QUuid>

#include "brokersessionstore.h"

class TestWindowsBrokerSessionStore : public QObject
{
    Q_OBJECT

private slots:
    void currentUserRoundTrip();
};

void TestWindowsBrokerSessionStore::currentUserRoundTrip()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString broker = QStringLiteral("test-broker-") + QUuid::createUuid().toString();
    BrokerSessionStore::Saved saved;
    QVERIFY(BrokerSessionStore::isAvailable());
    QVERIFY(BrokerSessionStore::save(broker, QStringLiteral("artist"),
                                     QStringLiteral("opaque-token-123")));
    QVERIFY(BrokerSessionStore::load(broker, saved));
    QCOMPARE(saved.username, QStringLiteral("artist"));
    QCOMPARE(saved.token, QStringLiteral("opaque-token-123"));
    QVERIFY(!BrokerSessionStore::load(broker + QStringLiteral("-other"), saved));
    BrokerSessionStore::clear(broker);
    QVERIFY(!BrokerSessionStore::load(broker, saved));
}

QTEST_GUILESS_MAIN(TestWindowsBrokerSessionStore)
#include "test_windowsbrokersessionstore.moc"
