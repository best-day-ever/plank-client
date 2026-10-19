#include <QtTest>
#include "fernwehupdatepolicy.h"

using namespace FernwehUpdatePolicy;

class TestFernwehUpdater : public QObject {
    Q_OBJECT
private slots:
    void feedIdentityAndVersion();
    void rejectsMalformedRelease_data();
    void rejectsMalformedRelease();
    void strictDownloadPaths();
};

static QJsonObject manifest(bool windows = true)
{
    QJsonObject object{{"version", "1.1.040"}, {"size", 100}, {"sha256", QString(64, 'a')},
        {"url", feedPath(windows) + (windows ? "/1.1.040.msi" : "/1.1.040.dmg")}};
    if (windows) {
        object["platform"] = "windows-x64";
        object["product_name"] = WindowsProduct;
        object["upgrade_code"] = WindowsUpgradeCode;
    }
    return object;
}

void TestFernwehUpdater::feedIdentityAndVersion()
{
    Release release;
    QVERIFY(parse(QJsonDocument(manifest()).toJson(), true, release));
    QCOMPARE(release.version, QString("1.1.040"));
    QCOMPARE(release.size, qint64(100));
    QVERIFY(!newer(release.version, "1.1.40")); // MSI canonicalizes zero padding.
    QVERIFY(newer(release.version, "1.1.039"));
    QVERIFY(!newer(release.version, "1.1.041"));
    QVERIFY(parse(QJsonDocument(manifest(false)).toJson(), false, release));
    QVERIFY(!parse(QJsonDocument(manifest(false)).toJson(), true, release));
    QVERIFY(!parse(QJsonDocument(manifest()).toJson(), false, release));
}

void TestFernwehUpdater::rejectsMalformedRelease_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QJsonValue>("value");
    QTest::newRow("external origin") << QString("url") << QJsonValue("https://example.test/release.msi");
    QTest::newRow("relative traversal") << QString("url") << QJsonValue("/v1/client-updates/windows-x64/../1.1.040.msi");
    QTest::newRow("wrong release") << QString("url") << QJsonValue("/v1/client-updates/windows-x64/1.1.041.msi");
    QTest::newRow("wrong platform") << QString("platform") << QJsonValue("windows-arm64");
    QTest::newRow("wrong product") << QString("product_name") << QJsonValue("Other app");
    QTest::newRow("wrong upgrade family") << QString("upgrade_code") << QJsonValue("{00000000-0000-0000-0000-000000000000}");
    QTest::newRow("invalid hash") << QString("sha256") << QJsonValue(QString(64, 'g'));
    QTest::newRow("missing hash") << QString("sha256") << QJsonValue();
    QTest::newRow("oversized") << QString("size") << QJsonValue(double(MaximumSize + 1));
    QTest::newRow("enormous") << QString("size") << QJsonValue(1e100);
    QTest::newRow("fractional") << QString("size") << QJsonValue(1.25);
    QTest::newRow("negative") << QString("size") << QJsonValue(-1);
    QTest::newRow("zero") << QString("size") << QJsonValue(0);
    QTest::newRow("string size") << QString("size") << QJsonValue("100");
    QTest::newRow("prerelease") << QString("version") << QJsonValue("1.1.040-unsafe");
    QTest::newRow("newline version") << QString("version") << QJsonValue("1.1.040\n");
    QTest::newRow("newline hash") << QString("sha256") << QJsonValue(QString(64, 'a') + '\n');
    QTest::newRow("MSI major overflow") << QString("version") << QJsonValue("256.1.040");
}

void TestFernwehUpdater::rejectsMalformedRelease()
{
    QFETCH(QString, key);
    QFETCH(QJsonValue, value);
    auto object = manifest();
    object[key] = value;
    Release release;
    release.version = "unchanged";
    QVERIFY(!parse(QJsonDocument(object).toJson(), true, release));
    QCOMPARE(release.version, QString("unchanged"));
}

void TestFernwehUpdater::strictDownloadPaths()
{
    QVERIFY(downloadPathAllowed("/v1/client-updates/windows-x64/1.1.040.msi"));
    QVERIFY(downloadPathAllowed("/v1/client-updates/macos-arm64/1.1.040.dmg"));
    for (const auto& path : {"/v1/client-updates/windows-x64/1.1.040.msi\n", "https://example.test/1.1.040.msi", "//example.test/release.msi",
         "/v1/client-updates/windows-x64/1.1.040.msi?other=1", "/v1/client-updates/windows-x64/1.1.040.msi/extra",
         "/v1/client-updates/windows-x64/1.1.040.dmg", "/v1/client-updates/macos-arm64/1.1.040.msi",
         "/v1/client-updates/windows-x64/%2e%2e/1.1.040.msi", "/v1/client-updates/windows-x64/../1.1.040.msi"}) {
        QVERIFY2(!downloadPathAllowed(QString::fromLatin1(path)), path);
    }
}

QTEST_APPLESS_MAIN(TestFernwehUpdater)
#include "test_fernwehupdater.moc"
