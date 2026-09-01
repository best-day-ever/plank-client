#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "plankclientpolicy.h"

class TestPlankClientPolicy : public QObject
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

void TestPlankClientPolicy::omittedValueIsNotManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    bool value = true;
    const PlankClientPolicy policy(directory.filePath(QStringLiteral("client.conf")));
    QVERIFY(!policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestPlankClientPolicy::explicitFalseIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), false);
    }

    bool value = true;
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

void TestPlankClientPolicy::explicitTrueIsManaged()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/mdns_discovery"), true);
    }

    bool value = false;
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(value);
}

void TestPlankClientPolicy::invalidValueFailsClosed()
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
    const PlankClientPolicy policy(path);
    QVERIFY(policy.managedBoolean(QStringLiteral("network/mdns_discovery"), &value));
    QVERIFY(!value);
}

void TestPlankClientPolicy::omittedPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const PlankClientPolicy policy(
                directory.filePath(QStringLiteral("client.conf")));
    QCOMPARE(policy.networkPort(), PlankClientPolicy::BuiltInNetworkPort);
}

void TestPlankClientPolicy::configuredPortIsReturned()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 31000);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), quint16(31000));
}

void TestPlankClientPolicy::invalidPortUsesBuiltInDefault()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("client.conf"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("network/port"), 70000);
    }

    const PlankClientPolicy policy(path);
    QCOMPARE(policy.networkPort(), PlankClientPolicy::BuiltInNetworkPort);
}

QTEST_APPLESS_MAIN(TestPlankClientPolicy)
#include "test_plankclientpolicy.moc"
