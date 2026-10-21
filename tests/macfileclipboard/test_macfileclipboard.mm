#include <QtTest>
#include <QTemporaryDir>
#include <deque>
#include <cstdio>
#include <memory>
#include "macfileclipboard.mm"

namespace {
std::unique_ptr<QTemporaryDir> testStaging;
std::string plankClipboardTestStagingRoot() { return testStaging->path().toStdString(); }
}

NSPasteboard* plankClipboardTestPasteboard()
{
    static NSPasteboard* board = [NSPasteboard pasteboardWithUniqueName];
    return board;
}

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
    bool cancelIncomingAtMaterialize = false;
    unsigned outgoingOfferCount = 0, clientCancels = 0;
    bool crossHostOffer = false, collisionInProgress = false, crossedHostPublished = false;
    QString protocolError;
    PlankClipboardFileOfferBegin outgoingBegin {};
    std::vector<OfferedEntry> outgoingEntries;
    std::array<uint8_t, 16> outgoingOffer {};
    uint64_t outgoingEpoch = 0, clientSequence = 1, hostSequence = 1;
    uint64_t materialization = 781;
    size_t outgoingIndex = 0;
    std::deque<std::vector<std::uint8_t>> incomingControl, incomingData, sentControl, sentData;
};

static void queueHostFile(PlankTransportNativeEndpoint& endpoint,
                          const std::string& name, const QByteArray& content);

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
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record)) return false;
    if (record.type == PLANK_CLIPBOARD_FILE_OFFER_BEGIN) {
        endpoint.clientSequence = 1; endpoint.hostSequence = 1;
        endpoint.outgoingEntries.clear(); endpoint.outgoingIndex = 0;
        endpoint.outgoingFinished = false;
        ++endpoint.outgoingOfferCount;
    }
    if (record.sequence != endpoint.clientSequence++) return false;
    if (record.type == PLANK_CLIPBOARD_FILE_OFFER_BEGIN) {
        std::memcpy(endpoint.outgoingOffer.data(), record.offer_id, 16);
        endpoint.outgoingEpoch = record.session_epoch;
        return !data && plank_clipboard_file_offer_begin_decode(record.payload,
                    record.payload_size, &endpoint.outgoingBegin) &&
                endpoint.outgoingBegin.direction == PLANK_CLIPBOARD_FILE_CLIENT_TO_HOST;
    }
    if (record.session_epoch != endpoint.outgoingEpoch ||
            std::memcmp(record.offer_id, endpoint.outgoingOffer.data(), 16)) return false;
    if (record.type == PLANK_CLIPBOARD_FILE_CANCEL) {
        ++endpoint.clientCancels;
        return !data;
    }
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
        if (endpoint.crossHostOffer && endpoint.outgoingOfferCount == 1) {
            // Deliberately do not ACK this first Client offer. The Host starts
            // its own offer on the same control lane, winning arbitration.
            endpoint.collisionInProgress = true;
            queueHostFile(endpoint, "host-cross.exr", "crossed host pixels");
            return true;
        }
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
    PlankClipboardFileRecord record {};
    const bool decoded = plank_clipboard_file_decode(bytes, size, data ?
            PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES + PLANK_CLIPBOARD_FILE_DATA_BYTES :
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record);
    const std::array<uint8_t, 16> crossedOffer {{42}};
    const bool crossedHostResponse = endpoint->collisionInProgress && decoded &&
            record.session_epoch == 1 && std::memcmp(record.offer_id, crossedOffer.data(), 16) == 0;
    if (endpoint->acceptOutgoing && !crossedHostResponse) {
        if (!receiveOutgoingRecord(*endpoint, bytes, size, data)) {
            endpoint->protocolError = QStringLiteral("Invalid outgoing PCF2 sequence or content");
            return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
        }
        return PLANK_TRANSPORT_OK;
    }
    if (crossedHostResponse && record.type == PLANK_CLIPBOARD_FILE_TRANSFER_READY) {
        endpoint->crossedHostPublished = true;
        endpoint->collisionInProgress = false;
        return PLANK_TRANSPORT_OK;
    }
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
        if (endpoint->cancelIncomingAtMaterialize) {
            PlankClipboardFileTerminal terminal {id, PLANK_CLIPBOARD_FILE_REASON_CANCELLED_BY_USER, 0};
            std::array<uint8_t, PLANK_CLIPBOARD_FILE_TERMINAL_BYTES> payload {};
            plank_clipboard_file_terminal_encode(payload.data(), &terminal);
            queue(PLANK_CLIPBOARD_FILE_CANCEL, payload.data(), payload.size(), false);
            return PLANK_TRANSPORT_OK;
        }
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
static void localText(NSString* text)
{
    auto* board = plankClipboardTestPasteboard();
    [board clearContents];
    [board setString:text forType:NSPasteboardTypeString];
}
static QString receivedPath()
{
    NSArray* urls = [plankClipboardTestPasteboard() readObjectsForClasses:@[[NSURL class]]
                    options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (urls.count != 1) return {};
    return QString::fromUtf8([[urls firstObject] fileSystemRepresentation]);
}
class TestMacFileClipboard : public QObject {
    Q_OBJECT
private slots:
    void init() { testStaging = std::make_unique<QTemporaryDir>(); QVERIFY(testStaging->isValid()); localText(@"original local copy"); }
    void sendsSelectedFilesAndDirectories() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("folder"));
        const auto path = directory.filePath("folder/frame.exr");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray content(300123, 'x'); QCOMPARE(file.write(content), qint64(content.size())); file.close();
        PlankTransportNativeEndpoint endpoint; endpoint.acceptOutgoing = true;
        std::atomic<int> ready {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [&] { ++ready; }, [] { return true; });
        clipboard.start();
        QVERIFY(clipboard.sendFilesOnMainThread({directory.filePath("folder").toStdString()}));
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Files sent") != std::string::npos, 4000);
        QCOMPARE(ready.load(), 0);
        QVERIFY(!clipboard.takeReadyPasteOnMainThread());
        QVERIFY(!clipboard.takeReadyPasteOnMainThread());
        clipboard.stop();
        QVERIFY2(endpoint.protocolError.isEmpty(), qPrintable(endpoint.protocolError));
        QCOMPARE(endpoint.outgoingEntries.size(), size_t(2));
        QCOMPARE(endpoint.outgoingEntries[1].received, content);
        QVERIFY(endpoint.outgoingEntries[1].complete);
    }
    void sendsFinderFileUrls() {
        QTemporaryDir directory;
        QFile file(directory.filePath("clip.txt")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Finder copy"); file.close();
        auto* board = plankClipboardTestPasteboard(); [board clearContents];
        QVERIFY([board writeObjects:@[[NSURL fileURLWithPath:QStringToNSString(file.fileName())]]]);
        PlankTransportNativeEndpoint endpoint; endpoint.acceptOutgoing = true;
        std::atomic<int> ready {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [&] { ++ready; }, [] { return true; });
        clipboard.start(); QVERIFY(clipboard.beginPasteOnMainThread());
        QTRY_COMPARE_WITH_TIMEOUT(ready.load(), 1, 4000);
        clipboard.stop(); QCOMPARE(endpoint.outgoingEntries[0].received, QByteArray("Finder copy"));
    }
    void receivesAndDoesNotEchoRemoteFiles() {
        PlankTransportNativeEndpoint endpoint;
        std::atomic<int> pending {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] { ++pending; return true; });
        clipboard.start(); clipboard.pollLocalClipboardOnMainThread();
        { std::lock_guard<std::mutex> lock(endpoint.mutex); queueHostFile(endpoint, "remote.exr", "remote pixels"); }
        QTRY_COMPARE_WITH_TIMEOUT(pending.load(), 1, 4000);
        QVERIFY(clipboard.publishPendingHostFilesOnMainThread());
        const auto path = receivedPath(); QVERIFY(!path.isEmpty());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("remote pixels")); file.close();
        QVERIFY(!clipboard.beginPasteOnMainThread());
        clipboard.stop();
        QFile::remove(path); QDir().rmdir(QFileInfo(path).absolutePath());
    }
    void newerLocalCopyWins() {
        PlankTransportNativeEndpoint endpoint; std::atomic<int> pending {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] { ++pending; return true; });
        clipboard.start(); clipboard.pollLocalClipboardOnMainThread();
        { std::lock_guard<std::mutex> lock(endpoint.mutex); queueHostFile(endpoint, "remote.exr", "pixels"); }
        QTRY_COMPARE_WITH_TIMEOUT(pending.load(), 1, 4000);
        localText(@"newer copy");
        QVERIFY(!clipboard.publishPendingHostFilesOnMainThread());
        QCOMPARE(QString::fromUtf8([[plankClipboardTestPasteboard() stringForType:NSPasteboardTypeString] UTF8String]), QString("newer copy"));
        clipboard.stop();
    }
    void corruptedHashDoesNotPublish() {
        PlankTransportNativeEndpoint endpoint; endpoint.corruptHash = true; std::atomic<int> pending {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] { ++pending; return true; });
        clipboard.start(); clipboard.pollLocalClipboardOnMainThread();
        { std::lock_guard<std::mutex> lock(endpoint.mutex); queueHostFile(endpoint, "bad.exr", "pixels"); }
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Receive failed") != std::string::npos, 4000);
        QCOMPARE(pending.load(), 0); clipboard.stop();
    }
    void focusLossCancelsDeferredPaste() {
        QTemporaryDir directory; QFile file(directory.filePath("clip.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("pixels"); file.close();
        PlankTransportNativeEndpoint endpoint; endpoint.acceptOutgoing = true; endpoint.automaticReady = false;
        std::atomic<int> ready {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [&] { ++ready; }, [] { return true; });
        auto* board = plankClipboardTestPasteboard(); [board clearContents];
        QVERIFY([board writeObjects:@[[NSURL fileURLWithPath:QStringToNSString(file.fileName())]]]);
        clipboard.start(); QVERIFY(clipboard.beginPasteOnMainThread());
        QTRY_VERIFY_WITH_TIMEOUT(([&] { std::lock_guard<std::mutex> lock(endpoint.mutex); return endpoint.outgoingFinished; })(), 4000);
        clipboard.cancelPendingPasteOnMainThread();
        { std::lock_guard<std::mutex> lock(endpoint.mutex);
          std::array<uint8_t, PLANK_CLIPBOARD_FILE_MATERIALIZE_BYTES> payload {};
          plank_clipboard_file_materialization_encode(payload.data(), endpoint.materialization);
          queueOutgoingReply(endpoint, PLANK_CLIPBOARD_FILE_TRANSFER_READY, 0, payload.data(), payload.size()); }
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Files sent") != std::string::npos, 4000);
        QCOMPARE(ready.load(), 0); QVERIFY(!clipboard.takeReadyPasteOnMainThread()); clipboard.stop();
    }
    void crossedOffersYieldThenRetrySelection() {
        QTemporaryDir directory; QFile file(directory.filePath("selected.exr"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("selected pixels"); file.close();
        PlankTransportNativeEndpoint endpoint; endpoint.acceptOutgoing = true; endpoint.crossHostOffer = true;
        std::atomic<int> pending {0};
        MacFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [&] { ++pending; return true; });
        clipboard.start(); QVERIFY(clipboard.sendFilesOnMainThread({file.fileName().toStdString()}));
        QTRY_COMPARE_WITH_TIMEOUT(pending.load(), 1, 4000);
        QVERIFY(clipboard.publishPendingHostFilesOnMainThread());
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Files sent") != std::string::npos, 4000);
        clipboard.stop(); QCOMPARE(endpoint.outgoingOfferCount, 2u);
        QCOMPARE(endpoint.outgoingEntries[0].received, QByteArray("selected pixels"));
        QVERIFY2(endpoint.protocolError.isEmpty(), qPrintable(endpoint.protocolError));
    }
    void malformedIncomingDoesNotStallWorker() {
        PlankTransportNativeEndpoint endpoint;
        MacFileClipboard clipboard(&endpoint, "bidirectional", [] {}, [] { return true; });
        clipboard.start(); clipboard.pollLocalClipboardOnMainThread();
        { std::lock_guard<std::mutex> lock(endpoint.mutex);
          queueHostFile(endpoint, "bad.exr", "pixels");
          endpoint.incomingControl[1][0] ^= 128; }
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Receive failed") != std::string::npos, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { std::lock_guard<std::mutex> lock(endpoint.mutex); return endpoint.incomingControl.empty(); })(), 4000);
        QTemporaryDir directory; QFile file(directory.filePath("next.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("next copy"); file.close();
        { std::lock_guard<std::mutex> lock(endpoint.mutex); endpoint.acceptOutgoing = true; }
        QVERIFY(clipboard.sendFilesOnMainThread({file.fileName().toStdString()}));
        QTRY_VERIFY_WITH_TIMEOUT(clipboard.status().find("Files sent") != std::string::npos, 4000);
        clipboard.stop();
    }
    void enforcesDirectionPolicy() {
        PlankTransportNativeEndpoint endpoint;
        MacFileClipboard clipboard(&endpoint, "host-to-client", [] {}, [] { return true; });
        clipboard.start(); QVERIFY(!clipboard.sendFilesOnMainThread({"/tmp/example"})); clipboard.stop();
    }
private:
    static NSString* QStringToNSString(const QString& value) {
        return [NSString stringWithUTF8String:value.toUtf8().constData()];
    }
};
QTEST_GUILESS_MAIN(TestMacFileClipboard)
#include "test_macfileclipboard.moc"
