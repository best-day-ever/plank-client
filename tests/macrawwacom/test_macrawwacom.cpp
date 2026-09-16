#include <QtTest>
#include "macrawwacomlogic.h"

class TestMacRawWacom : public QObject {
    Q_OBJECT
private slots:
    void frames();
    void malformed();
    void reportTypesAndIds();
};

static QByteArray frame(unsigned type, unsigned size)
{
    PLANK_RAW_HID_WIRE_HEADER h{};
    h.magic = qToLittleEndian(std::uint32_t(PLANK_RAW_HID_WIRE_MAGIC));
    h.version = qToLittleEndian(std::uint16_t(PLANK_RAW_HID_WIRE_VERSION));
    h.generation = qToLittleEndian(std::uint16_t(7));
    h.interfaceId = qToLittleEndian(std::uint16_t(1));
    h.transactionId = qToLittleEndian(std::uint32_t(42));
    h.type = qToLittleEndian(std::uint16_t(type));
    h.payloadLength = qToLittleEndian(std::uint32_t(size));
    QByteArray bytes(reinterpret_cast<const char*>(&h), sizeof(h));
    bytes.append(QByteArray(size, 0));
    return bytes;
}
static bool parse(const QByteArray& bytes, MacWacomWire::Control& out)
{
    return MacWacomWire::parse(reinterpret_cast<const unsigned char*>(bytes.constData()), bytes.size(), out);
}
void TestMacRawWacom::frames()
{
    MacWacomWire::Control c;
    QVERIFY(parse(frame(PLANK_RAW_HID_GET_REPORT, 2), c));
    QCOMPARE(c.interfaceId, 1); QCOMPARE(c.generation, 7); QCOMPARE(c.transaction, 42U);
    QVERIFY(parse(frame(PLANK_RAW_HID_ATTACH_RESULT, 4), c));
    QVERIFY(parse(frame(PLANK_RAW_HID_SET_REPORT, 4097), c));
    QVERIFY(parse(frame(PLANK_RAW_HID_OUTPUT, 2), c));
}
void TestMacRawWacom::malformed()
{
    MacWacomWire::Control c;
    QVERIFY(!MacWacomWire::parse(nullptr, 100, c));
    const auto good = frame(PLANK_RAW_HID_GET_REPORT, 2);
    for (int n = 0; n < good.size(); ++n) QVERIFY(!parse(good.left(n), c));
    QVERIFY(!parse(good + 'x', c));
    auto bad = good; bad[0] ^= 1; QVERIFY(!parse(bad, c));
    bad = good; bad[4] = 1; QVERIFY(!parse(bad, c));
    bad = good; bad[10] = 0; QVERIFY(!parse(bad, c));
    bad = good; bad[8] = 16; QVERIFY(!parse(bad, c));
    QVERIFY(!parse(frame(PLANK_RAW_HID_INPUT, 2), c));
    QVERIFY(!parse(frame(PLANK_RAW_HID_GET_REPORT, 3), c));
    QVERIFY(!parse(frame(PLANK_RAW_HID_ATTACH_RESULT, 3), c));
    QVERIFY(!parse(frame(PLANK_RAW_HID_SET_REPORT, 1), c));
    QVERIFY(!parse(frame(PLANK_RAW_HID_SET_REPORT, 4098), c));
}
void TestMacRawWacom::reportTypesAndIds()
{
    QCOMPARE(MacWacomWire::ioReportType(0), 2); // UHID feature -> IOKit feature
    QCOMPARE(MacWacomWire::ioReportType(1), 1);
    QCOMPARE(MacWacomWire::ioReportType(2), 0);
    QCOMPARE(MacWacomWire::ioReportType(3), -1);
    QCOMPARE(MacWacomWire::reportPrefix(0), std::size_t(1));
    QCOMPARE(MacWacomWire::reportPrefix(16), std::size_t(0));
}
QTEST_APPLESS_MAIN(TestMacRawWacom)
#include "test_macrawwacom.moc"
