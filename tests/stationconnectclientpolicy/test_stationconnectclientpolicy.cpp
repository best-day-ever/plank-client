#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "stationconnectclientpolicy.h"

class TestStationConnectClientPolicy : public QObject
{
    Q_OBJECT

private slots:
    void omittedValueIsNotManaged();
    void explicitFalseIsManaged();
    void explicitTrueIsManaged();
    void invalidValueFailsClosed();
};

void TestStationConnectClientPolicy::omittedValueIsNotManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    bool value = true;
    const StationConnectClientPolicy policy(directory.filePath(QStringLiteral("client.conf")));
    QVERIFY(!policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestStationConnectClientPolicy::explicitFalseIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), false);
    }

    bool value = true;
    const StationConnectClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

void TestStationConnectClientPolicy::explicitTrueIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), true);
    }

    bool value = false;
    const StationConnectClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestStationConnectClientPolicy::invalidValueFailsClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"),
                          QStringLiteral("sometimes"));
    }

    bool value = true;
    const StationConnectClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

QTEST_APPLESS_MAIN(TestStationConnectClientPolicy)
#include "test_stationconnectclientpolicy.moc"
