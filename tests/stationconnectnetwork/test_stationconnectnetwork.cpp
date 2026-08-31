#include <QtTest>

#include "stationconnectnetwork.h"

class TestStationConnectNetwork : public QObject
{
    Q_OBJECT

private slots:
    void recognizesLinuxZeroTierInterface();
    void recognizesNamedZeroTierInterface();
    void rejectsPhysicalAndMalformedInterfaces();
    void keepsQuicDatagramsInsideZeroTierMtu();
};

void TestStationConnectNetwork::recognizesLinuxZeroTierInterface()
{
    QVERIFY(StationConnectNetwork::isZeroTierInterface(QStringLiteral("ztk4jiikvl"),
                                                        QStringLiteral("ztk4jiikvl")));
}

void TestStationConnectNetwork::recognizesNamedZeroTierInterface()
{
    QVERIFY(StationConnectNetwork::isZeroTierInterface(QStringLiteral("iftype53_1"),
                                                        QStringLiteral("ZeroTier One")));
}

void TestStationConnectNetwork::rejectsPhysicalAndMalformedInterfaces()
{
    QVERIFY(!StationConnectNetwork::isZeroTierInterface(QStringLiteral("enp86s0"),
                                                         QStringLiteral("enp86s0")));
    QVERIFY(!StationConnectNetwork::isZeroTierInterface(QStringLiteral("ztshort"),
                                                         QStringLiteral("ztshort")));
    QVERIFY(!StationConnectNetwork::isZeroTierInterface(QStringLiteral("ztbad_name"),
                                                         QStringLiteral("ztbad_name")));
}

void TestStationConnectNetwork::keepsQuicDatagramsInsideZeroTierMtu()
{
    QCOMPARE(StationConnectNetwork::ZeroTierQuicUdpPayloadMtu, quint16(1344));
    QCOMPARE(StationConnectNetwork::ZeroTierQuicApplicationDatagramSize, quint16(1306));
    QCOMPARE(StationConnectNetwork::ZeroTierRaptorQVideoSymbolSize, quint16(1280));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(0, true), quint16(1344));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(0, false), quint16(1200));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1500, false), quint16(1452));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1500, true), quint16(1432));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 1420, false), quint16(1372));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(
                 0, false, 9000, false), quint16(1452));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(1280, true), quint16(1280));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(1452, false), quint16(1452));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(1199, true), quint16(1344));
    QCOMPARE(StationConnectNetwork::quicUdpPayloadMtuForRoute(65528, false), quint16(1200));
    QCOMPARE(StationConnectNetwork::ZeroTierQuicUdpPayloadMtu +
                 StationConnectNetwork::InnerIpv4UdpOverhead + 38,
             1410);
    QCOMPARE(StationConnectNetwork::ZeroTierQuicUdpPayloadMtu +
                 StationConnectNetwork::InnerIpv4UdpOverhead +
                 StationConnectNetwork::ZeroTierExtendedFrameOverhead,
             1423);
    QVERIFY(StationConnectNetwork::ZeroTierQuicUdpPayloadMtu +
                StationConnectNetwork::InnerIpv4UdpOverhead +
                StationConnectNetwork::ZeroTierExtendedFrameOverhead <=
            StationConnectNetwork::ZeroTierPhysicalUdpPayloadLimit);
}

QTEST_APPLESS_MAIN(TestStationConnectNetwork)
#include "test_stationconnectnetwork.moc"
