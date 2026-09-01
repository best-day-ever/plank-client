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
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(1199, true), quint16(1344));
    QCOMPARE(PlankNetwork::quicUdpPayloadMtuForRoute(65528, false), quint16(1200));
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

QTEST_APPLESS_MAIN(TestPlankNetwork)
#include "test_planknetwork.moc"
