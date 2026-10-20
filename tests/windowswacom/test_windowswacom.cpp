#include <QtTest>
#include "windowswacomlogic.h"
#include "macrawwacomlogic.h"
#include "linuxwacomreportworker.h"
#include <atomic>

class TestWindowsWacom : public QObject {
    Q_OBJECT
private slots:
    void reportSizesPreserveZeroBytesAndStripOnlyPadding() {
        const unsigned char descriptor[] = {
            0x85, 2, 0x75, 8, 0x95, 3, 0x81, 2, // ID 2: three bytes + ID
            0x85, 3, 0x95, 7, 0x81, 2,          // ID 3: seven bytes + ID
            0x85, 4, 0x95, 2, 0xb1, 2};         // Feature ID 4: two + ID
        WindowsWacom::ReportSizes sizes;
        QVERIFY(WindowsWacom::reportSizes(descriptor, sizeof(descriptor), sizes));
        QVERIFY(sizes.numbered);
        QCOMPARE(sizes.bytes[0][2], std::size_t(4));
        QCOMPARE(sizes.bytes[0][3], std::size_t(8));
        QCOMPARE(sizes.bytes[2][4], std::size_t(3));
        const unsigned char padded[] = {2, 0x80, 0, 0, 0, 0, 0, 0};
        QCOMPARE(WindowsWacom::inputLength(sizes, padded, sizeof(padded)), std::size_t(4));
        QCOMPARE(WindowsWacom::inputLength(sizes, padded, 3), std::size_t(0));
        const unsigned char unknown[] = {5};
        QCOMPARE(WindowsWacom::inputLength(sizes, unknown, 1), std::size_t(0));
    }
    void unnumberedAndFeatureOnlyCollections() {
        const unsigned char descriptor[] = {0x75, 1, 0x95, 9, 0x81, 2};
        WindowsWacom::ReportSizes sizes;
        QVERIFY(WindowsWacom::reportSizes(descriptor, sizeof(descriptor), sizes));
        QVERIFY(!sizes.numbered);
        const unsigned char input[] = {0, 0}; // zero is data, not a synthetic ID
        QCOMPARE(WindowsWacom::inputLength(sizes, input, 2), std::size_t(2));
        const unsigned char feature[] = {0x85, 5, 0x75, 8, 0x95, 4, 0xb1, 2};
        QVERIFY(WindowsWacom::reportSizes(feature, sizeof(feature), sizes));
        QCOMPARE(sizes.bytes[2][5], std::size_t(5));
    }
    void globalPushPopAndConstantPadding() {
        const unsigned char descriptor[] = {
            0x85, 1, 0x75, 8, 0x95, 1, 0xa4, // Push global state
            0x75, 1, 0x95, 3, 0x81, 2,       // Three bits
            0x95, 5, 0x81, 3,                // Five constant bits
            0xb4, 0x81, 2};                  // Pop, add eight bits
        WindowsWacom::ReportSizes sizes;
        QVERIFY(WindowsWacom::reportSizes(descriptor, sizeof(descriptor), sizes));
        QCOMPARE(sizes.bytes[0][1], std::size_t(3));
    }
    void rejectsMalformedAndOversizedDescriptors() {
        const std::vector<std::vector<unsigned char>> invalid = {
            {0x75}, {0xb4}, {0xa4}, {0xfe, 4}, {0xfe, 4, 0, 1},
            {0x85, 0}, {0x86, 0, 1},
            {0x75, 8, 0x97, 0xff, 0xff, 0xff, 0xff, 0x81, 2},
            {0x75, 8, 0x95, 1, 0x81, 2, 0x85, 1, 0x81, 2}};
        for (const auto& descriptor : invalid) {
            WindowsWacom::ReportSizes sizes;
            QVERIFY(!WindowsWacom::reportSizes(descriptor.data(), descriptor.size(), sizes));
        }
    }
    void firstGenerationNeedsNormalizedFallback() {
        QVERIFY(WindowsWacom::normalizedOnlyProduct(0x0314));
        QVERIFY(WindowsWacom::normalizedOnlyProduct(0x0315));
        QVERIFY(WindowsWacom::normalizedOnlyProduct(0x0317));
        QVERIFY(!WindowsWacom::normalizedOnlyProduct(0x0357));
    }
    void hostControlsAreBoundedAndValidated() {
        PLANK_RAW_HID_WIRE_HEADER header{};
        header.magic = qToLittleEndian<std::uint32_t>(PLANK_RAW_HID_WIRE_MAGIC);
        header.version = qToLittleEndian<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION);
        header.type = qToLittleEndian<std::uint16_t>(PLANK_RAW_HID_GET_REPORT);
        header.generation = qToLittleEndian<std::uint16_t>(1);
        header.payloadLength = qToLittleEndian<std::uint32_t>(2);
        std::vector<unsigned char> bytes(sizeof(header) + 2);
        std::memcpy(bytes.data(), &header, sizeof(header));
        MacWacomWire::Control control;
        QVERIFY(MacWacomWire::parse(bytes.data(), bytes.size(), control));
        QVERIFY(!MacWacomWire::parse(bytes.data(), bytes.size() - 1, control));
        header.generation = 0;
        std::memcpy(bytes.data(), &header, sizeof(header));
        QVERIFY(!MacWacomWire::parse(bytes.data(), bytes.size(), control));
        header.generation = qToLittleEndian<std::uint16_t>(1);
        header.interfaceId = qToLittleEndian<std::uint16_t>(PLANK_RAW_HID_MAX_INTERFACES);
        std::memcpy(bytes.data(), &header, sizeof(header));
        QVERIFY(!MacWacomWire::parse(bytes.data(), bytes.size(), control));
    }
    void reconnectDiscardsStaleFeatureCompletion() {
        LinuxWacomReportWorker worker;
        std::atomic_bool entered{false}, finish{false};
        QVERIFY(worker.submit([&] {
            entered = true;
            while (!finish) std::this_thread::yield();
            return LinuxWacomReportWorker::Result{1, 0, 7, {1}};
        }));
        QTRY_VERIFY(entered.load());
        worker.invalidate();
        finish = true;
        QVERIFY(worker.submit([] { return LinuxWacomReportWorker::Result{1, 0, 8, {2}}; }));
        std::deque<LinuxWacomReportWorker::Result> results;
        QTRY_VERIFY(([&] { if (results.empty()) results = worker.take(); return !results.empty(); })());
        QCOMPARE(results.size(), std::size_t(1));
        QCOMPARE(results.front().transaction, std::uint32_t(8));
    }
};
QTEST_APPLESS_MAIN(TestWindowsWacom)
#include "test_windowswacom.moc"
