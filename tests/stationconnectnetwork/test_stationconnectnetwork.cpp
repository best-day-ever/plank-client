#include <QtTest>

#include "stationconnectnetwork.h"
#include "stationconnectpacketsize.h"

class TestStationConnectNetwork : public QObject
{
    Q_OBJECT

private slots:
    void recognizesLinuxZeroTierInterface();
    void recognizesNamedZeroTierInterface();
    void rejectsPhysicalAndMalformedInterfaces();
    void keepsEncryptedVideoInsideZeroTierMtu();
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

void TestStationConnectNetwork::keepsEncryptedVideoInsideZeroTierMtu()
{
    QCOMPARE(StationConnectPacketSize::VpnVideoPacketSize, 1328);
    QCOMPARE(StationConnectPacketSize::sunshineNegotiatedPacketSize(1328), 1296);
    QCOMPARE(StationConnectPacketSize::sunshineEncryptedUdpPayload(1328), 1344);
    QCOMPARE(StationConnectPacketSize::zeroTierPhysicalPayload(1328), 1410);
    QCOMPARE(StationConnectPacketSize::zeroTierPhysicalPayload(
                 1328, StationConnectPacketSize::ZeroTierExtendedFrameOverhead),
             1423);
    QVERIFY(StationConnectPacketSize::zeroTierPhysicalPayload(1328) <=
            StationConnectPacketSize::ZeroTierPhysicalPayloadMtu);
    QVERIFY(StationConnectPacketSize::zeroTierPhysicalPayload(1392) >
            StationConnectPacketSize::ZeroTierPhysicalPayloadMtu);
}

QTEST_APPLESS_MAIN(TestStationConnectNetwork)
#include "test_stationconnectnetwork.moc"
