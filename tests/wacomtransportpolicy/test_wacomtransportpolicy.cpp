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
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0314),
             PlankWacomTransport::NormalizedPen);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0315),
             PlankWacomTransport::NormalizedPen);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0317),
             PlankWacomTransport::NormalizedPen);
}

void TestWacomTransportPolicy::usesExactRawHidForNewerWacoms()
{
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0357),
             PlankWacomTransport::ExactRawHid);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0358),
             PlankWacomTransport::ExactRawHid);
    QCOMPARE(plankWacomTransportForUsbDevice(0x056a, 0x0400),
             PlankWacomTransport::ExactRawHid);
}

void TestWacomTransportPolicy::doesNotApplyWacomFallbackToAnotherVendor()
{
    QCOMPARE(plankWacomTransportForUsbDevice(0x1234, 0x0315),
             PlankWacomTransport::ExactRawHid);
}

QTEST_APPLESS_MAIN(TestWacomTransportPolicy)
#include "test_wacomtransportpolicy.moc"
