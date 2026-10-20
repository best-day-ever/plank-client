#include <QtTest>
#include "windowsfileclipboardpaths.h"

#ifdef Q_OS_WIN
// Compile the actual platform implementation; the private native transport is
// replaced at its C ABI by deterministic, bounded in-memory queues below.
#include "windowsfileclipboard.cpp"
#include <deque>
#include <cwchar>

struct PlankTransportNativeEndpoint {
    std::mutex mutex;
    QByteArray hostContent;
    bool corruptHash = false;
    std::deque<std::vector<std::uint8_t>> incomingControl, incomingData, sentControl, sentData;
};

static int32_t sendRecord(PlankTransportNativeEndpoint* endpoint, const uint8_t* bytes,
                          size_t size, bool data)
{
    std::lock_guard<std::mutex> lock(endpoint->mutex);
    (data ? endpoint->sentData : endpoint->sentControl).emplace_back(bytes, bytes + size);
    PlankClipboardFileRecord record {};
    if (!data && plank_clipboard_file_decode(bytes, size,
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record) &&
            record.type == PLANK_CLIPBOARD_FILE_MATERIALIZE) {
        uint64_t id = 0;
        if (!plank_clipboard_file_materialization_decode(record.payload, record.payload_size, &id))
            return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
        uint64_t sequence = 4;
        auto queue = [&](uint16_t type, const uint8_t* payload, size_t count, bool content) {
            std::vector<uint8_t> wire(PLANK_CLIPBOARD_FILE_HEADER_BYTES + count);
            plank_clipboard_file_header(wire.data(), wire.size(), type, 0, uint32_t(count),
                    record.offer_id, record.session_epoch, sequence++);
            std::memcpy(wire.data() + PLANK_CLIPBOARD_FILE_HEADER_BYTES, payload, count);
            (content ? endpoint->incomingData : endpoint->incomingControl).push_back(std::move(wire));
        };
        if (!endpoint->hostContent.isEmpty()) {
            std::vector<uint8_t> chunk(PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES +
                                       size_t(endpoint->hostContent.size()));
            plank_clipboard_file_range_header(chunk.data(), id, 1, 0,
                                               uint32_t(endpoint->hostContent.size()));
            std::memcpy(chunk.data() + PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES,
                        endpoint->hostContent.constData(), size_t(endpoint->hostContent.size()));
            queue(PLANK_CLIPBOARD_FILE_DATA, chunk.data(), chunk.size(), true);
        }
        PlankClipboardFileEntryComplete complete {id, 1, uint64_t(endpoint->hostContent.size()), {0}};
        const auto hash = QCryptographicHash::hash(endpoint->hostContent, QCryptographicHash::Sha256);
        std::memcpy(complete.sha256, hash.constData(), 32);
        if (endpoint->corruptHash) complete.sha256[0] ^= 1;
        std::array<uint8_t, PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE_BYTES> payload {};
        plank_clipboard_file_entry_complete_encode(payload.data(), &complete);
        queue(PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE, payload.data(), payload.size(), false);
    }
    return PLANK_TRANSPORT_OK;
}
static int32_t receiveRecord(PlankTransportNativeEndpoint* endpoint, uint8_t* bytes,
                             size_t capacity, size_t* size, uint32_t timeout, bool data)
{
    {
        std::lock_guard<std::mutex> lock(endpoint->mutex);
        auto& queue = data ? endpoint->incomingData : endpoint->incomingControl;
        if (!queue.empty()) {
            if (queue.front().size() > capacity) return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
            *size = queue.front().size();
            std::memcpy(bytes, queue.front().data(), *size);
            queue.pop_front();
            return PLANK_TRANSPORT_OK;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout, uint32_t {5})));
    return PLANK_TRANSPORT_TIMEOUT;
}
extern "C" int32_t plank_transport_native_file_control_send(PlankTransportNativeEndpoint* e,
        const uint8_t* bytes, size_t size) { return sendRecord(e, bytes, size, false); }
extern "C" int32_t plank_transport_native_file_data_send(PlankTransportNativeEndpoint* e,
        const uint8_t* bytes, size_t size) { return sendRecord(e, bytes, size, true); }
extern "C" int32_t plank_transport_native_file_control_receive(PlankTransportNativeEndpoint* e,
        uint8_t* bytes, size_t cap, size_t* size, uint32_t timeout) {
    return receiveRecord(e, bytes, cap, size, timeout, false);
}
extern "C" int32_t plank_transport_native_file_data_receive(PlankTransportNativeEndpoint* e,
        uint8_t* bytes, size_t cap, size_t* size, uint32_t timeout) {
    return receiveRecord(e, bytes, cap, size, timeout, true);
}

static void queueHostFile(PlankTransportNativeEndpoint& endpoint, const std::string& name,
                          const QByteArray& content)
{
    endpoint.hostContent = content;
    const std::array<uint8_t, 16> offer {{42}};
    uint64_t sequence = 1;
    auto record = [&](uint16_t type, const uint8_t* bytes, size_t size, bool data = false) {
        std::vector<uint8_t> result(PLANK_CLIPBOARD_FILE_HEADER_BYTES + size);
        plank_clipboard_file_header(result.data(), result.size(), type, 0,
                                    uint32_t(size), offer.data(), 1, sequence++);
        if (size) std::memcpy(result.data() + PLANK_CLIPBOARD_FILE_HEADER_BYTES, bytes, size);
        (data ? endpoint.incomingData : endpoint.incomingControl).push_back(std::move(result));
    };
    std::array<uint8_t, PLANK_CLIPBOARD_FILE_OFFER_BEGIN_BYTES> beginBytes {};
    PlankClipboardFileOfferBegin begin {PLANK_CLIPBOARD_FILE_HOST_TO_CLIENT, 1, 1, 1,
                                        uint64_t(content.size())};
    plank_clipboard_file_offer_begin_encode(beginBytes.data(), &begin);
    record(PLANK_CLIPBOARD_FILE_OFFER_BEGIN, beginBytes.data(), beginBytes.size());
    PlankClipboardFileOfferEntry entry {1, 0, PLANK_CLIPBOARD_FILE_REGULAR, 0600,
        uint64_t(content.size()), 1700000000000000000LL,
        reinterpret_cast<const uint8_t*>(name.data()), uint16_t(name.size())};
    std::array<uint8_t, PLANK_CLIPBOARD_FILE_OFFER_ENTRY_PREFIX_BYTES + 255> entryBytes {};
    size_t entrySize = 0;
    plank_clipboard_file_offer_entry_encode(entryBytes.data(), entryBytes.size(), &entry, &entrySize);
    record(PLANK_CLIPBOARD_FILE_OFFER_ENTRY, entryBytes.data(), entrySize);
    record(PLANK_CLIPBOARD_FILE_OFFER_END, nullptr, 0);

}
#endif

#ifdef Q_OS_WIN
static bool setLocalText(const wchar_t* text)
{
    HWND owner = CreateWindowExW(0, L"STATIC", L"clipboard fixture", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return false;
    const auto size = (std::wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    bool success = false;
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            std::memcpy(data, text, size);
            GlobalUnlock(memory);
            if (OpenClipboard(owner)) {
                success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
                CloseClipboard();
            }
        }
        if (!success) GlobalFree(memory);
    }
    DestroyWindow(owner);
    return success;
}

static uint16_t lastType(PlankTransportNativeEndpoint& endpoint)
{
    std::lock_guard<std::mutex> lock(endpoint.mutex);
    if (endpoint.sentControl.empty()) return 0;
    const auto& bytes = endpoint.sentControl.back();
    PlankClipboardFileRecord record {};
    if (!plank_clipboard_file_decode(bytes.data(), bytes.size(),
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record)) return 0;
    return record.type;
}
#endif

class TestWindowsFileClipboard : public QObject {
    Q_OBJECT
private slots:
    void rejectsWindowsAliases_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {QStringLiteral("CON"), QStringLiteral("nul.txt"),
                QStringLiteral("COM1.mov"), QStringLiteral("LPT9"), QStringLiteral("COM\u00b9"),
                QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"), QStringLiteral("file:stream"),
                QStringLiteral("folder\\child"), QStringLiteral("../escape"),
                QStringLiteral("trailing."), QStringLiteral("trailing "), QStringLiteral("a?b"),
                QStringLiteral("a\nb"), QStringLiteral(".")})
            QTest::newRow(qPrintable(name)) << name;
    }
    void rejectsWindowsAliases()
    {
        QFETCH(QString, name);
        QVERIFY(!plank::clipboard::windows::validName(name.toUtf8().toStdString()));
    }
    void acceptsRegularUnicodeNames()
    {
        for (const auto* name : {"render.exr", "COM10.mov", "nulldata", "caf\xc3\xa9.exr"})
            QVERIFY(plank::clipboard::windows::validName(name));
    }
    void foldsUnicodeCaseCollisions()
    {
        QCOMPARE(plank::clipboard::windows::foldedPath("folder/RENDER.EXR"),
                 plank::clipboard::windows::foldedPath("FOLDER/render.exr"));
        QCOMPARE(plank::clipboard::windows::foldedPath("\xc3\x84.exr"),
                 plank::clipboard::windows::foldedPath("\xc3\xa4.exr"));
    }
#ifdef Q_OS_WIN
    void freezesManifestSourceAndRejectsHardlinks()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto path = temporary.path() + QStringLiteral("/render.exr");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("pixels"), qint64 {6});
        file.close();
        std::map<fs::path, NativeHandle> ancestors;
        std::vector<SourceEntry> entries;
        uint64_t bytes = 0;
        QVERIFY(appendTree(fs::path(path.toStdWString()), 0, 1, entries, bytes, ancestors));
        QCOMPARE(bytes, uint64_t {6});
        QVERIFY(!file.open(QIODevice::WriteOnly));
        QVERIFY(!QFile::rename(path, path + QStringLiteral(".moved")));
        QVERIFY(statStable(entries.front().handle.get(), entries.front()));
        entries.clear(); ancestors.clear();
        QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        const auto alias = path + QStringLiteral(".alias");
        QVERIFY(CreateHardLinkW(alias.toStdWString().c_str(), path.toStdWString().c_str(), nullptr));
        QVERIFY(!appendTree(fs::path(path.toStdWString()), 0, 1, entries, bytes, ancestors));
    }
    void receivesHashedFileAndCleansUpOnDisconnect_data()
    {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("valid") << false;
        QTest::newRow("hash-mismatch") << true;
    }
    void receivesHashedFileAndCleansUpOnDisconnect()
    {
        QFETCH(bool, corrupt);
        PlankTransportNativeEndpoint endpoint;
        endpoint.corruptHash = corrupt;
        queueHostFile(endpoint, "render.exr", "remote pixels");
        std::atomic_bool requested {false};
        WindowsFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] {
            requested = true; return true;
        });
        clipboard.start();
        if (corrupt) {
            QTRY_VERIFY_WITH_TIMEOUT(lastType(endpoint) == PLANK_CLIPBOARD_FILE_ERROR, 2000);
            clipboard.stop();
            QVERIFY(!requested);
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(requested.load(), 2000);
        QVERIFY(clipboard.publishPendingHostFilesOnMainThread());
        QVERIFY(OpenClipboard(nullptr));
        const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        const auto length = drop ? DragQueryFileW(drop, 0, nullptr, 0) : 0;
        std::vector<wchar_t> path(length + 1);
        if (drop) DragQueryFileW(drop, 0, path.data(), length + 1);
        CloseClipboard();
        QVERIFY(length > 0);
        const auto received = QString::fromWCharArray(path.data());
        QFile file(received);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("remote pixels"));
        file.close();
        QTRY_VERIFY_WITH_TIMEOUT(lastType(endpoint) == PLANK_CLIPBOARD_FILE_TRANSFER_READY, 2000);
        // A published host drop must never become a new outbound offer.
        QVERIFY(!clipboard.beginPasteOnMainThread());
        clipboard.stop();
        QVERIFY(!QFile::exists(received));
    }
    void preservesLocalClipboardWhenIncomingPublicationIsDenied_data()
    {
        QTest::addColumn<bool>("localCopy");
        QTest::newRow("stream-unfocused") << false;
        QTest::newRow("new-local-copy") << true;
    }
    void preservesLocalClipboardWhenIncomingPublicationIsDenied()
    {
        QFETCH(bool, localCopy);
        PlankTransportNativeEndpoint endpoint;
        queueHostFile(endpoint, "render.exr", "remote pixels");
        std::atomic_bool requested {false};
        WindowsFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] {
            requested = true; return true;
        }, [localCopy] { return localCopy; });
        clipboard.start();
        QTRY_VERIFY_WITH_TIMEOUT(requested.load(), 2000);
        if (localCopy) QVERIFY(setLocalText(L"new local copy"));
        const auto sequence = GetClipboardSequenceNumber();
        QVERIFY(!clipboard.publishPendingHostFilesOnMainThread());
        QCOMPARE(GetClipboardSequenceNumber(), sequence);
        QTRY_VERIFY_WITH_TIMEOUT(lastType(endpoint) == PLANK_CLIPBOARD_FILE_ERROR, 2000);
        clipboard.stop();
        QCOMPARE(GetClipboardSequenceNumber(), sequence);
    }
    void rejectsReservedHostNameWithoutPublication()
    {
        PlankTransportNativeEndpoint endpoint;
        queueHostFile(endpoint, "CON.txt", "pixels");
        std::atomic_bool published {false};
        WindowsFileClipboard clipboard(&endpoint, "host-to-client", [] {}, [&] {
            published = true; return true;
        });
        clipboard.start();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            std::lock_guard<std::mutex> lock(endpoint.mutex);
            return !endpoint.sentControl.empty();
        })(), 2000);
        clipboard.stop();
        QVERIFY(!published);
        PlankClipboardFileRecord rejected {};
        QVERIFY(plank_clipboard_file_decode(endpoint.sentControl.back().data(),
                endpoint.sentControl.back().size(), PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT,
                &rejected));
        QCOMPARE(rejected.type, uint16_t {PLANK_CLIPBOARD_FILE_ERROR});
    }
#endif
};
QTEST_GUILESS_MAIN(TestWindowsFileClipboard)
#include "test_windowsfileclipboard.moc"
