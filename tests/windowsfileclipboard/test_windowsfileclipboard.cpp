#include <QtTest>
#include "windowsfileclipboardpaths.h"

#ifdef Q_OS_WIN
// Compile the actual platform implementation; the private native transport is
// replaced at its C ABI by deterministic, bounded in-memory queues below.
#include "windowsfileclipboard.cpp"
#include <deque>
#include <cwchar>

struct OfferedEntry {
    uint64_t id = 0, parent = 0, size = 0;
    uint8_t kind = 0;
    std::string name;
    QByteArray received;
    bool complete = false;
};

struct PlankTransportNativeEndpoint {
    std::mutex mutex;
    QByteArray hostContent;
    bool corruptHash = false;
    bool acceptOutgoing = false, cancelOutgoing = false, automaticReady = true;
    bool outgoingFinished = false;
    QString protocolError;
    PlankClipboardFileOfferBegin outgoingBegin {};
    std::vector<OfferedEntry> outgoingEntries;
    std::array<uint8_t, 16> outgoingOffer {};
    uint64_t outgoingEpoch = 0, clientSequence = 1, hostSequence = 1;
    uint64_t materialization = 781;
    size_t outgoingIndex = 0;
    std::deque<std::vector<std::uint8_t>> incomingControl, incomingData, sentControl, sentData;
};

// A small Host-side PCF2 receiver. It chooses materialization/ranges itself,
// validates Client sequences, reconstructs bytes, and checks complete hashes.
// All calls below run while the fixture endpoint mutex is held.
static bool queueOutgoingReply(PlankTransportNativeEndpoint& endpoint, uint16_t type,
                                uint32_t flags, const uint8_t* bytes, size_t size)
{
    std::vector<uint8_t> wire(PLANK_CLIPBOARD_FILE_HEADER_BYTES + size);
    if (!plank_clipboard_file_header(wire.data(), wire.size(), type, flags, uint32_t(size),
            endpoint.outgoingOffer.data(), endpoint.outgoingEpoch, endpoint.hostSequence++))
        return false;
    if (size) std::memcpy(wire.data() + PLANK_CLIPBOARD_FILE_HEADER_BYTES, bytes, size);
    endpoint.incomingControl.push_back(std::move(wire));
    return true;
}

static bool requestOutgoingRange(PlankTransportNativeEndpoint& endpoint)
{
    while (endpoint.outgoingIndex < endpoint.outgoingEntries.size() &&
            endpoint.outgoingEntries[endpoint.outgoingIndex].kind == PLANK_CLIPBOARD_FILE_DIRECTORY)
        ++endpoint.outgoingIndex;
    if (endpoint.outgoingIndex == endpoint.outgoingEntries.size()) {
        endpoint.outgoingFinished = true;
        if (!endpoint.automaticReady) return true;
        std::array<uint8_t, PLANK_CLIPBOARD_FILE_MATERIALIZE_BYTES> ready {};
        return plank_clipboard_file_materialization_encode(ready.data(), endpoint.materialization) &&
                queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_TRANSFER_READY, 0,
                                    ready.data(), ready.size());
    }
    const auto& entry = endpoint.outgoingEntries[endpoint.outgoingIndex];
    const auto offset = uint64_t(entry.received.size());
    if (offset == entry.size) return true; // empty/finished entry awaits ENTRY_COMPLETE
    // Use smaller ranges than the client maximum, so the source must honor the
    // receiver's requested sizes and offsets over several content records.
    const auto amount = uint32_t(std::min<uint64_t>(65521, entry.size - offset));
    std::array<uint8_t, PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES> request {};
    return plank_clipboard_file_range_header(request.data(), endpoint.materialization,
                entry.id, offset, amount) &&
            queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_READ_REQUEST, 0,
                                request.data(), request.size());
}

static bool receiveOutgoingRecord(PlankTransportNativeEndpoint& endpoint,
                                  const uint8_t* bytes, size_t size, bool data)
{
    PlankClipboardFileRecord record {};
    if (!plank_clipboard_file_decode(bytes, size, data ?
            PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES + PLANK_CLIPBOARD_FILE_DATA_BYTES :
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record) ||
            record.sequence != endpoint.clientSequence++) return false;
    if (record.type == PLANK_CLIPBOARD_FILE_OFFER_BEGIN) {
        std::memcpy(endpoint.outgoingOffer.data(), record.offer_id, 16);
        endpoint.outgoingEpoch = record.session_epoch;
        return !data && plank_clipboard_file_offer_begin_decode(record.payload,
                    record.payload_size, &endpoint.outgoingBegin) &&
                endpoint.outgoingBegin.direction == PLANK_CLIPBOARD_FILE_CLIENT_TO_HOST;
    }
    if (record.session_epoch != endpoint.outgoingEpoch ||
            std::memcmp(record.offer_id, endpoint.outgoingOffer.data(), 16)) return false;
    if (record.type == PLANK_CLIPBOARD_FILE_OFFER_ENTRY) {
        PlankClipboardFileOfferEntry entry {};
        if (data || !plank_clipboard_file_offer_entry_decode(record.payload,
                    record.payload_size, &entry)) return false;
        OfferedEntry saved;
        saved.id = entry.entry_id; saved.parent = entry.parent_id;
        saved.size = entry.size; saved.kind = entry.kind;
        saved.name.assign(reinterpret_cast<const char*>(entry.name), entry.name_size);
        endpoint.outgoingEntries.push_back(std::move(saved));
        return true;
    }
    if (record.type == PLANK_CLIPBOARD_FILE_OFFER_END) {
        if (data || record.payload_size || endpoint.outgoingEntries.size() !=
                endpoint.outgoingBegin.entry_count) return false;
        if (endpoint.cancelOutgoing) {
            PlankClipboardFileTerminal terminal {0, PLANK_CLIPBOARD_FILE_REASON_CANCELLED_BY_USER, 0};
            std::array<uint8_t, PLANK_CLIPBOARD_FILE_TERMINAL_BYTES> payload {};
            return plank_clipboard_file_terminal_encode(payload.data(), &terminal) &&
                    queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_CANCEL, 0,
                                        payload.data(), payload.size());
        }
        std::array<uint8_t, PLANK_CLIPBOARD_FILE_OFFER_ACK_BYTES> ack {};
        std::array<uint8_t, PLANK_CLIPBOARD_FILE_MATERIALIZE_BYTES> materialize {};
        return plank_clipboard_file_offer_ack_encode(ack.data(), PLANK_CLIPBOARD_FILE_DATA_BYTES,
                    PLANK_CLIPBOARD_FILE_MAX_OUTSTANDING_BYTES) &&
                queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_OFFER_ACK, 0, ack.data(), ack.size()) &&
                plank_clipboard_file_materialization_encode(materialize.data(), endpoint.materialization) &&
                queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_MATERIALIZE,
                        PLANK_CLIPBOARD_FILE_FLAG_ALL_ITEMS, materialize.data(), materialize.size()) &&
                requestOutgoingRange(endpoint);
    }
    if (endpoint.outgoingIndex >= endpoint.outgoingEntries.size()) return false;
    auto& entry = endpoint.outgoingEntries[endpoint.outgoingIndex];
    if (record.type == PLANK_CLIPBOARD_FILE_DATA) {
        PlankClipboardFileRange range {};
        if (!data || !plank_clipboard_file_decode_range(record.payload, record.payload_size, 1, &range) ||
                range.materialization_id != endpoint.materialization || range.entry_id != entry.id ||
                range.offset != uint64_t(entry.received.size()) ||
                range.size != uint32_t(std::min<uint64_t>(65521, entry.size - range.offset)))
            return false;
        entry.received.append(reinterpret_cast<const char*>(range.bytes), range.size);
        return requestOutgoingRange(endpoint);
    }
    if (record.type == PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE) {
        PlankClipboardFileEntryComplete complete {};
        const auto digest = QCryptographicHash::hash(entry.received, QCryptographicHash::Sha256);
        if (data || !plank_clipboard_file_entry_complete_decode(record.payload,
                    record.payload_size, &complete) ||
                complete.materialization_id != endpoint.materialization || complete.entry_id != entry.id ||
                complete.size != entry.size || uint64_t(entry.received.size()) != entry.size ||
                std::memcmp(complete.sha256, digest.constData(), 32)) return false;
        entry.complete = true;
        ++endpoint.outgoingIndex;
        return requestOutgoingRange(endpoint);
    }
    return false;
}

static int32_t sendRecord(PlankTransportNativeEndpoint* endpoint, const uint8_t* bytes,
                          size_t size, bool data)
{
    std::lock_guard<std::mutex> lock(endpoint->mutex);
    (data ? endpoint->sentData : endpoint->sentControl).emplace_back(bytes, bytes + size);
    if (endpoint->acceptOutgoing) {
        if (!receiveOutgoingRecord(*endpoint, bytes, size, data)) {
            endpoint->protocolError = QStringLiteral("Invalid outgoing PCF2 sequence or content");
            return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
        }
        return PLANK_TRANSPORT_OK;
    }
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
static bool setLocalFiles(const QStringList& paths)
{
    HWND owner = CreateWindowExW(0, L"STATIC", L"file clipboard fixture", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return false;
    std::vector<wchar_t> names;
    for (const auto& path : paths) {
        const auto native = QDir::toNativeSeparators(path).toStdWString();
        names.insert(names.end(), native.begin(), native.end());
        names.push_back(0);
    }
    names.push_back(0);
    const size_t bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    bool success = false;
    if (memory) {
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop) {
            drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
            std::memcpy(reinterpret_cast<uint8_t*>(drop) + sizeof(DROPFILES),
                        names.data(), names.size() * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (OpenClipboard(owner)) {
                success = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
                CloseClipboard();
            }
        }
        if (!success) GlobalFree(memory);
    }
    DestroyWindow(owner);
    return success;
}

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
    void sendsLocalFolderWithHierarchyChunksAndEmptyFiles()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QDir root(temporary.path());
        QVERIFY(root.mkpath(QStringLiteral("frames/nested")));
        const auto folder = root.filePath(QStringLiteral("frames"));
        const auto imagePath = root.filePath(QStringLiteral("frames/nested/caf\u00e9.exr"));
        QByteArray pixels(PLANK_CLIPBOARD_FILE_DATA_BYTES + 137, Qt::Uninitialized);
        for (qsizetype index = 0; index < pixels.size(); ++index) pixels[index] = char(index % 251);
        QFile image(imagePath);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write(pixels), qint64(pixels.size())); image.close();
        QFile empty(root.filePath(QStringLiteral("frames/empty.txt")));
        QVERIFY(empty.open(QIODevice::WriteOnly)); empty.close();
        QFile loose(root.filePath(QStringLiteral("loose.txt")));
        QVERIFY(loose.open(QIODevice::WriteOnly)); QCOMPARE(loose.write("separate"), qint64 {8}); loose.close();
        QVERIFY(setLocalFiles({folder, loose.fileName()}));
        PlankTransportNativeEndpoint endpoint;
        endpoint.acceptOutgoing = true;
        endpoint.automaticReady = false;
        std::atomic_int ready {0};
        WindowsFileClipboard clipboard(&endpoint, "bidirectional", [&] { ++ready; }, [] { return false; });
        clipboard.start();
        QVERIFY(clipboard.beginPasteOnMainThread());
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            std::lock_guard<std::mutex> lock(endpoint.mutex);
            return endpoint.outgoingFinished || !endpoint.protocolError.isEmpty();
        })(), 3000);
        QCOMPARE(ready.load(), 0); // The withheld paste awaits TRANSFER_READY.
        {
            std::lock_guard<std::mutex> lock(endpoint.mutex);
            QVERIFY2(endpoint.protocolError.isEmpty(), qPrintable(endpoint.protocolError));
            QCOMPARE(endpoint.outgoingBegin.top_level_count, uint32_t {2});
            QCOMPARE(endpoint.outgoingBegin.total_bytes, uint64_t(pixels.size() + 8));
            QCOMPARE(endpoint.outgoingEntries.size(), size_t {5});
            std::map<uint64_t, QString> paths;
            std::map<QString, QByteArray> contents;
            for (const auto& entry : endpoint.outgoingEntries) {
                const auto name = QString::fromUtf8(entry.name.data(), qsizetype(entry.name.size()));
                if (entry.parent) QVERIFY(paths.count(entry.parent));
                const auto path = entry.parent ? paths.at(entry.parent) + u'/' + name : name;
                paths.emplace(entry.id, path);
                if (entry.kind == PLANK_CLIPBOARD_FILE_REGULAR) {
                    QVERIFY(entry.complete);
                    contents.emplace(path, entry.received);
                }
            }
            QVERIFY(contents.count(QStringLiteral("frames/nested/caf\u00e9.exr")));
            QVERIFY(contents.count(QStringLiteral("frames/empty.txt")));
            QVERIFY(contents.count(QStringLiteral("loose.txt")));
            QCOMPARE(contents.at(QStringLiteral("frames/nested/caf\u00e9.exr")), pixels);
            QCOMPARE(contents.at(QStringLiteral("frames/empty.txt")), QByteArray {});
            QCOMPARE(contents.at(QStringLiteral("loose.txt")), QByteArray("separate"));
            QVERIFY(endpoint.sentData.size() > 2); // receiver-selected multi-chunk ranges
            std::array<uint8_t, PLANK_CLIPBOARD_FILE_MATERIALIZE_BYTES> payload {};
            QVERIFY(plank_clipboard_file_materialization_encode(payload.data(), endpoint.materialization));
            QVERIFY(queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_TRANSFER_READY, 0,
                                        payload.data(), payload.size()));
        }
        QTRY_COMPARE_WITH_TIMEOUT(ready.load(), 1, 2000);
        clipboard.stop();
        QVERIFY(setLocalText(L"file test complete"));
    }
    void cancelledOutgoingOfferNeverReleasesPaste()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile file(temporary.filePath(QStringLiteral("cancel.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("pixels"), qint64 {6}); file.close();
        QVERIFY(setLocalFiles({file.fileName()}));
        PlankTransportNativeEndpoint endpoint;
        endpoint.acceptOutgoing = true;
        endpoint.cancelOutgoing = true;
        std::atomic_int ready {0};
        WindowsFileClipboard clipboard(&endpoint, "client-to-host", [&] { ++ready; }, [] { return false; });
        clipboard.start();
        QVERIFY(clipboard.beginPasteOnMainThread());
        QTRY_COMPARE_WITH_TIMEOUT(lastType(endpoint), uint16_t {PLANK_CLIPBOARD_FILE_OFFER_END}, 2000);
        // Cancellation must release all frozen source handles, without waiting
        // for stop or releasing the remote paste gesture.
        QTRY_VERIFY_WITH_TIMEOUT(file.open(QIODevice::WriteOnly), 2000);
        file.close();
        QCOMPARE(ready.load(), 0);
        clipboard.stop();
        QVERIFY(endpoint.sentData.empty());
        QVERIFY(endpoint.protocolError.isEmpty());
        QVERIFY(setLocalText(L"file test complete"));
    }
    void disallowedOutgoingModeDoesNotConsumePaste_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("host-to-client-only") << QStringLiteral("host-to-client");
        QTest::newRow("disabled") << QStringLiteral("disabled");
    }
    void disallowedOutgoingModeDoesNotConsumePaste()
    {
        QFETCH(QString, mode);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QFile file(temporary.filePath(QStringLiteral("private.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QVERIFY(setLocalFiles({file.fileName()}));
        PlankTransportNativeEndpoint endpoint;
        std::atomic_int ready {0};
        WindowsFileClipboard clipboard(&endpoint, mode.toStdString(), [&] { ++ready; }, [] { return false; });
        clipboard.start();
        QVERIFY(!clipboard.beginPasteOnMainThread());
        clipboard.stop();
        QCOMPARE(ready.load(), 0);
        QVERIFY(endpoint.sentControl.empty());
        QVERIFY(endpoint.sentData.empty());
        QVERIFY(setLocalText(L"file test complete"));
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
