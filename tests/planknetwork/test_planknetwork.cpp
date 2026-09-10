#include <QtTest>

#include "planknetwork.h"

class TestPlankNetwork : public QObject
{
    Q_OBJECT

private slots:
    void recognizesLinuxZeroTierInterface();
    void recognizesNamedZeroTierInterface();
    void rejectsPhysicalAndMalformedInterfaces();
    void keepsQuicDatagramsInsideZeroTierMtu();
    void selectsSafePayload_data();
    void selectsSafePayload();
    void neverExceedsKnownInterface();
};

void TestPlankNetwork::recognizesLinuxZeroTierInterface()
{
    QVERIFY(PlankNetwork::isZeroTierInterface(QStringLiteral("ztk4jiikvl"),
                                                        QStringLiteral("ztk4jiikvl")));
}

void TestPlankNetwork::recognizesNamedZeroTierInterface()
{
    QVERIFY(PlankNetwork::isZeroTierInterface(QStringLiteral("iftype53_1"),
                                                        QStringLiteral("ZeroTier One")));
}

void TestPlankNetwork::rejectsPhysicalAndMalformedInterfaces()
{
    QVERIFY(!PlankNetwork::isZeroTierInterface(QStringLiteral("enp86s0"),
                                                         QStringLiteral("enp86s0")));
    QVERIFY(!PlankNetwork::isZeroTierInterface(QStringLiteral("ztshort"),
                                                         QStringLiteral("ztshort")));
    QVERIFY(!PlankNetwork::isZeroTierInterface(QStringLiteral("ztbad_name"),
                                                         QStringLiteral("ztbad_name")));
}

void TestPlankNetwork::keepsQuicDatagramsInsideZeroTierMtu()
{
    QCOMPARE(PlankNetwork::ZeroTierQuicUdpPayloadMtu, quint16(1344));
    QCOMPARE(PlankNetwork::ZeroTierQuicApplicationDatagramSize, quint16(1306));
    QCOMPARE(PlankNetwork::ZeroTierRaptorQVideoSymbolSize, quint16(1280));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(0, true), quint16(1344));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(0, false), quint16(1200));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1500, false), quint16(1452));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1500, true), quint16(1432));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1420, false), quint16(1372));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 9000, false), quint16(1452));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(1280, true), quint16(1280));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(1452, false), quint16(1452));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(1199, true), quint16(0));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(65528, false), quint16(0));
    QCOMPARE(PlankNetwork::ZeroTierQuicUdpPayloadMtu +
                 PlankNetwork::InnerIpv4UdpOverhead + 38,
             1410);
    QCOMPARE(PlankNetwork::ZeroTierQuicUdpPayloadMtu +
                 PlankNetwork::InnerIpv4UdpOverhead +
                 PlankNetwork::ZeroTierExtendedFrameOverhead,
             1423);
    QVERIFY(PlankNetwork::ZeroTierQuicUdpPayloadMtu +
                PlankNetwork::InnerIpv4UdpOverhead +
                PlankNetwork::ZeroTierExtendedFrameOverhead <=
            PlankNetwork::ZeroTierPhysicalUdpPayloadLimit);
}

void TestPlankNetwork::selectsSafePayload_data()
{
    QTest::addColumn<int>("configured");
    QTest::addColumn<bool>("zeroTier");
    QTest::addColumn<quint32>("mtu");
    QTest::addColumn<bool>("ipv6");
    QTest::addColumn<quint16>("expected");
    QTest::newRow("zt-1316-v4") << 0 << true << quint32(1316) << false << quint16(1268);
    QTest::newRow("zt-1310-v4") << 0 << true << quint32(1310) << false << quint16(1262);
    QTest::newRow("zt-1310-v6") << 0 << true << quint32(1310) << true << quint16(1242);
    QTest::newRow("zt-2800-v4") << 0 << true << quint32(2800) << false << quint16(1344);
    QTest::newRow("zt-2800-v6") << 0 << true << quint32(2800) << true << quint16(1344);
    QTest::newRow("zt-1200") << 0 << true << quint32(1200) << false << quint16(0);
    QTest::newRow("minimum-v4") << 0 << true << quint32(1248) << false << quint16(1200);
    QTest::newRow("below-minimum-v4") << 0 << true << quint32(1247) << false << quint16(0);
    QTest::newRow("minimum-v6") << 0 << true << quint32(1268) << true << quint16(1200);
    QTest::newRow("below-minimum-v6") << 0 << true << quint32(1267) << true << quint16(0);
    QTest::newRow("manual-exact") << 1262 << true << quint32(1310) << false << quint16(1262);
    QTest::newRow("manual-over") << 1263 << true << quint32(1310) << false << quint16(0);
    QTest::newRow("manual-too-small") << 1199 << true << quint32(2800) << false << quint16(0);
    QTest::newRow("manual-too-large") << 65528 << false << quint32(0) << false << quint16(0);
    QTest::newRow("manual-negative") << -1 << true << quint32(0) << false << quint16(0);
    QTest::newRow("manual-unknown") << 1280 << true << quint32(0) << false << quint16(1280);
    QTest::newRow("manual-overrides-automatic-cap") << 1400 << true << quint32(2800) << false << quint16(1400);
    QTest::newRow("manual-unusable-interface") << 1200 << true << quint32(1200) << false << quint16(0);
    QTest::newRow("unknown-zt") << 0 << true << quint32(0) << false << quint16(1344);
    QTest::newRow("unknown-other") << 0 << false << quint32(0) << false << quint16(1200);
    QTest::newRow("ordinary-v6") << 0 << false << quint32(1500) << true << quint16(1432);
    QTest::newRow("ordinary-small") << 0 << false << quint32(1310) << false << quint16(1262);
    QTest::newRow("ordinary-unusable") << 0 << false << quint32(1200) << true << quint16(0);
    QTest::newRow("underflow") << 0 << true << quint32(1) << false << quint16(0);
    QTest::newRow("jumbo") << 0 << false << quint32(9000) << false << quint16(1452);
}

void TestPlankNetwork::selectsSafePayload()
{
    QFETCH(int, configured);
    QFETCH(bool, zeroTier);
    QFETCH(quint32, mtu);
    QFETCH(bool, ipv6);
    QFETCH(quint16, expected);
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(configured, zeroTier, mtu, ipv6), expected);
}

void TestPlankNetwork::neverExceedsKnownInterface()
{
    for (bool zeroTier : {false, true}) {
        for (bool ipv6 : {false, true}) {
            const quint32 overhead = (ipv6 ? 48 : 28) + 20;
            for (quint32 mtu = 1; mtu <= 9000; ++mtu) {
                for (int configured : {0, 1200, 1262, 1344, 1452}) {
                    const quint16 payload = PlankNetwork::quicUdpPayloadMtuForRoute(
                                configured, zeroTier, mtu, ipv6);
                    if (payload) {
                        QVERIFY(payload >= 1200);
                        QVERIFY(quint32(payload) + overhead <= mtu);
                        if (!configured && zeroTier) QVERIFY(payload <= 1344);
                    }
                    else {
                        QVERIFY(mtu < overhead + 1200 || (configured && quint32(configured) + overhead > mtu));
                    }
                }
            }
        }
    }
}

QTEST_APPLESS_MAIN(TestPlankNetwork)
#include "test_planknetwork.moc"
