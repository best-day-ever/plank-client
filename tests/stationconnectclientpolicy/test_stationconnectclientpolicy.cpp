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
    void omittedPortUsesBuiltInDefault();
    void configuredPortIsReturned();
    void invalidPortUsesBuiltInDefault();
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

void TestStationConnectClientPolicy::omittedPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const StationConnectClientPolicy policy(
                directory.filePath(QStringLiteral("client.conf")));
    QCOMPARE(policy.networkPort(), StationConnectClientPolicy::BuiltInNetworkPort);
}

void TestStationConnectClientPolicy::configuredPortIsReturned()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 31000);
    }

    const StationConnectClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), quint16(31000));
}

void TestStationConnectClientPolicy::invalidPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 70000);
    }

    const StationConnectClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), StationConnectClientPolicy::BuiltInNetworkPort);
}

QTEST_APPLESS_MAIN(TestStationConnectClientPolicy)
#include "test_stationconnectclientpolicy.moc"
