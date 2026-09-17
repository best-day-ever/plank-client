#include <QtTest>
#include "macrawwacomasync.h"
#include "macrawwacomlogic.h"
#include <memory>

class TestMacRawWacom : public QObject {
    Q_OBJECT
private slots:
    void frames();
    void malformed();
    void reportTypesAndIds();
    void stalledReportAcrossFocusReconnectAndQuit();
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
void TestMacRawWacom::stalledReportAcrossFocusReconnectAndQuit()
{
    auto results = std::make_shared<MacWacomAsyncResults>();
    std::weak_ptr<MacWacomAsyncResults> callbackTarget = results;
    MacWacomReleaseBarrier release;
    const auto stalledFocusEpoch = results->epoch();

    // A physical report callback has not arrived. Focus loss must advance the
    // lease without waiting for it, and a later completion cannot reach Host.
    const auto focusTicket = release.request();
    QVERIFY(!release.wait(focusTicket, std::chrono::milliseconds(10)));
    QVERIFY(!release.idle());
    results->invalidate();
    release.complete(focusTicket);
    QVERIFY(release.wait(focusTicket, std::chrono::milliseconds(0)));
    MacWacomAsyncResults::Completion lateFocus{};
    lateFocus.epoch = stalledFocusEpoch;
    lateFocus.transaction = 1;
    results->publish(std::move(lateFocus));
    QVERIFY(results->take().empty());

    const auto stalledReconnectEpoch = results->epoch();
    const auto reconnectTicket = release.request();
    QVERIFY(!release.wait(reconnectTicket, std::chrono::milliseconds(10)));
    results->invalidate();
    release.complete(reconnectTicket);
    MacWacomAsyncResults::Completion lateReconnect{};
    lateReconnect.epoch = stalledReconnectEpoch;
    lateReconnect.transaction = 2;
    results->publish(std::move(lateReconnect));
    QVERIFY(results->take().empty());

    MacWacomAsyncResults::Completion current{};
    current.epoch = results->epoch();
    current.transaction = 3;
    results->publish(std::move(current));
    const auto ready = results->take();
    QCOMPARE(ready.size(), std::size_t(1));
    QCOMPARE(ready.front().transaction, 3U);

    // Quit can destroy the mailbox while the OS still owns a request context.
    // Its eventual callback holds only a weak reference and must do nothing.
    const auto quitTicket = release.request();
    QVERIFY(!release.wait(quitTicket, std::chrono::milliseconds(10)));
    results.reset();
    QVERIFY(callbackTarget.expired());
    if (auto target = callbackTarget.lock()) QFAIL("Late callback reached a dead Client");
    release.markExited();
    QVERIFY(release.waitExited(std::chrono::milliseconds(0)));
}
QTEST_APPLESS_MAIN(TestMacRawWacom)
#include "test_macrawwacom.moc"
