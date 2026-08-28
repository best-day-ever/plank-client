#include <QtTest>

#include "linuxrawwacom.h"

class TestWacomTransportPolicy : public QObject
{
    Q_OBJECT

private slots:
    void usesNormalizedPenForFirstGenerationIntuosPro();
    void usesExactRawHidForNewerWacoms();
    void doesNotApplyWacomFallbackToAnotherVendor();
};

void TestWacomTransportPolicy::usesNormalizedPenForFirstGenerationIntuosPro()
{
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0314),
             StationConnectWacomTransport::NormalizedPen);
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0315),
             StationConnectWacomTransport::NormalizedPen);
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0317),
             StationConnectWacomTransport::NormalizedPen);
}

void TestWacomTransportPolicy::usesExactRawHidForNewerWacoms()
{
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0357),
             StationConnectWacomTransport::ExactRawHid);
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0358),
             StationConnectWacomTransport::ExactRawHid);
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x056a, 0x0400),
             StationConnectWacomTransport::ExactRawHid);
}

void TestWacomTransportPolicy::doesNotApplyWacomFallbackToAnotherVendor()
{
    QCOMPARE(stationConnectWacomTransportForUsbDevice(0x1234, 0x0315),
             StationConnectWacomTransport::ExactRawHid);
}

QTEST_APPLESS_MAIN(TestWacomTransportPolicy)
#include "test_wacomtransportpolicy.moc"
