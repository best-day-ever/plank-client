#include "macfileclipboard.h"

#include <AppKit/AppKit.h>
#include <QCryptographicHash>
#include <SDL3/SDL.h>
#include <Security/Security.h>
#include <plank_clipboard_file_wire.h>
#include <plank_transport.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

struct SourceEntry {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::uint8_t kind = 0;
    std::uint16_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtimeNs = 0;
    std::int64_t ctimeNs = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::string name;
    fs::path path;
};

struct SinkEntry {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::uint8_t kind = 0;
    std::uint16_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtimeNs = 0;
    unsigned depth = 0;
    std::string name;
    fs::path relative;
};

std::int64_t timespecNs(const timespec& value)
{
    constexpr std::int64_t Billion = 1000000000;
    if (value.tv_sec > std::numeric_limits<std::int64_t>::max() / Billion ||
            value.tv_sec < std::numeric_limits<std::int64_t>::min() / Billion) {
        return value.tv_sec < 0 ? std::numeric_limits<std::int64_t>::min() :
                                 std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value.tv_sec) * Billion + value.tv_nsec;
}

bool statStable(const struct stat& value, const SourceEntry& entry)
{
    return static_cast<std::uint64_t>(value.st_dev) == entry.device &&
            static_cast<std::uint64_t>(value.st_ino) == entry.inode &&
            static_cast<std::uint64_t>(value.st_size) == entry.size &&
            timespecNs(value.st_mtimespec) == entry.mtimeNs &&
            timespecNs(value.st_ctimespec) == entry.ctimeNs;
}

bool appendTree(const fs::path& path, std::uint64_t parent, unsigned depth,
                std::vector<SourceEntry>& entries, std::uint64_t& totalBytes)
{
    if (depth > PLANK_CLIPBOARD_FILE_MAX_DEPTH ||
            entries.size() >= PLANK_CLIPBOARD_FILE_MAX_ENTRIES) {
        return false;
    }
    struct stat value {};
    if (::lstat(path.c_str(), &value) != 0 || S_ISLNK(value.st_mode)) {
        return false;
    }
    const bool regular = S_ISREG(value.st_mode);
    const bool directory = S_ISDIR(value.st_mode);
    if (!regular && !directory) {
        return false;
    }
    if (regular && value.st_nlink > 1) {
        return false;
    }
    const std::string name = path.filename().u8string();
    if (!plank_clipboard_file_name_valid(
                reinterpret_cast<const std::uint8_t*>(name.data()), name.size())) {
        return false;
    }
    SourceEntry entry;
    entry.id = entries.size() + 1;
    entry.parent = parent;
    entry.kind = regular ? PLANK_CLIPBOARD_FILE_REGULAR :
                           PLANK_CLIPBOARD_FILE_DIRECTORY;
    entry.mode = static_cast<std::uint16_t>(value.st_mode & 0700);
    entry.size = regular ? static_cast<std::uint64_t>(value.st_size) : 0;
    entry.mtimeNs = timespecNs(value.st_mtimespec);
    entry.ctimeNs = timespecNs(value.st_ctimespec);
    entry.device = static_cast<std::uint64_t>(value.st_dev);
    entry.inode = static_cast<std::uint64_t>(value.st_ino);
    entry.name = name;
    entry.path = path;
    if (entry.size > std::numeric_limits<std::uint64_t>::max() - totalBytes) {
        return false;
    }
    totalBytes += entry.size;
    const auto entryId = entry.id;
    entries.push_back(std::move(entry));

    if (directory) {
        std::error_code error;
        std::vector<fs::path> children;
        for (fs::directory_iterator iterator(path, error), end;
             !error && iterator != end; iterator.increment(error)) {
            children.push_back(iterator->path());
        }
        if (error) {
            return false;
        }
        std::sort(children.begin(), children.end());
        for (const auto& child : children) {
            if (!appendTree(child, entryId, depth + 1, entries, totalBytes)) {
                return false;
            }
        }
    }
    return true;
}

class WireSession {
public:
    WireSession(PlankTransportNativeEndpoint* endpoint,
                const std::array<std::uint8_t, 16>& offer,
                std::uint64_t epoch,
                const std::atomic_bool& stop,
                std::uint64_t receiveSequence = 1)
        : m_Endpoint(endpoint), m_Offer(offer), m_Epoch(epoch), m_Stop(stop),
          m_ReceiveSequence(receiveSequence) {}

    bool sendControl(std::uint16_t type, std::uint32_t flags,
                     const std::uint8_t* payload, std::size_t size)
    {
        if (size > PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT) return false;
        std::vector<std::uint8_t> record(PLANK_CLIPBOARD_FILE_HEADER_BYTES + size);
        if (!plank_clipboard_file_header(record.data(), record.size(), type, flags,
                static_cast<std::uint32_t>(size), m_Offer.data(), m_Epoch, m_SendSequence++)) {
            return false;
        }
        if (size != 0) {
            std::memcpy(record.data() + PLANK_CLIPBOARD_FILE_HEADER_BYTES, payload, size);
        }
        return send(&plank_transport_native_file_control_send, record);
    }

    bool sendData(const std::uint8_t* payload, std::size_t size)
    {
        if (size > PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES +
                       PLANK_CLIPBOARD_FILE_DATA_BYTES) return false;
        std::vector<std::uint8_t> record(PLANK_CLIPBOARD_FILE_HEADER_BYTES + size);
        if (!plank_clipboard_file_header(record.data(), record.size(),
                PLANK_CLIPBOARD_FILE_DATA, 0, static_cast<std::uint32_t>(size),
                m_Offer.data(), m_Epoch, m_SendSequence++)) {
            return false;
        }
        std::memcpy(record.data() + PLANK_CLIPBOARD_FILE_HEADER_BYTES, payload, size);
        return send(&plank_transport_native_file_data_send, record);
    }

    std::optional<PlankClipboardFileRecord> receiveControl(
            std::vector<std::uint8_t>& storage, std::uint32_t timeoutMs)
    {
        return receive(storage, timeoutMs, false);
    }

    std::optional<PlankClipboardFileRecord> receiveData(
            std::vector<std::uint8_t>& storage, std::uint32_t timeoutMs)
    {
        return receive(storage, timeoutMs, true);
    }

    bool failed() const { return m_Failed; }

private:
    std::optional<PlankClipboardFileRecord> receive(
            std::vector<std::uint8_t>& storage, std::uint32_t timeoutMs,
            bool content)
    {
        const auto payloadLimit = content ?
                PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES +
                    PLANK_CLIPBOARD_FILE_DATA_BYTES :
                PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT;
        storage.resize(PLANK_CLIPBOARD_FILE_HEADER_BYTES + payloadLimit);
        std::size_t size = 0;
        const auto result = content ?
                plank_transport_native_file_data_receive(
                    m_Endpoint, storage.data(), storage.size(), &size, timeoutMs) :
                plank_transport_native_file_control_receive(
                    m_Endpoint, storage.data(), storage.size(), &size, timeoutMs);
        if (result == PLANK_TRANSPORT_TIMEOUT) return std::nullopt;
        if (result != PLANK_TRANSPORT_OK) {
            m_Failed = true;
            return std::nullopt;
        }
        storage.resize(size);
        PlankClipboardFileRecord record {};
        if (!plank_clipboard_file_decode(storage.data(), storage.size(),
                static_cast<std::uint32_t>(payloadLimit), &record) ||
                std::memcmp(record.offer_id, m_Offer.data(), m_Offer.size()) != 0 ||
                record.session_epoch != m_Epoch ||
                record.sequence != m_ReceiveSequence++) {
            m_Failed = true;
            return std::nullopt;
        }
        return record;
    }
    using SendFunction = int32_t (*)(PlankTransportNativeEndpoint*,
                                     const std::uint8_t*, std::size_t);
    bool send(SendFunction function, const std::vector<std::uint8_t>& record)
    {
        while (!m_Stop.load(std::memory_order_acquire)) {
            const auto result = function(m_Endpoint, record.data(), record.size());
            if (result == PLANK_TRANSPORT_OK) return true;
            if (result != PLANK_TRANSPORT_TIMEOUT) return false;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    PlankTransportNativeEndpoint* m_Endpoint;
    std::array<std::uint8_t, 16> m_Offer;
    std::uint64_t m_Epoch;
    const std::atomic_bool& m_Stop;
    std::uint64_t m_SendSequence = 1;
    std::uint64_t m_ReceiveSequence = 1;
    bool m_Failed = false;
};

bool secureRandom(void* bytes, std::size_t size)
{
    return SecRandomCopyBytes(kSecRandomDefault, size,
                              static_cast<std::uint8_t*>(bytes)) == errSecSuccess;
}

std::string asciiFold(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

fs::path stagingRoot()
{
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return fs::u8path(home) / "Library" / "Caches" / "PLANK" / "FileClipboard";
}

std::string offerName(const std::array<std::uint8_t, 16>& offer)
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(offer.size() * 2);
    for (const auto byte : offer) {
        result.push_back(Hex[byte >> 4]);
        result.push_back(Hex[byte & 0x0f]);
    }
    return result;
}

bool setFileMtime(int fd, std::int64_t nanoseconds)
{
    constexpr std::int64_t Billion = 1000000000;
    timespec times[2] {};
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = nanoseconds / Billion;
    times[1].tv_nsec = nanoseconds % Billion;
    if (times[1].tv_nsec < 0) {
        --times[1].tv_sec;
        times[1].tv_nsec += Billion;
    }
    return ::futimens(fd, times) == 0;
}

bool setPathMtime(const fs::path& path, std::int64_t nanoseconds)
{
    constexpr std::int64_t Billion = 1000000000;
    timespec times[2] {};
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = nanoseconds / Billion;
    times[1].tv_nsec = nanoseconds % Billion;
    if (times[1].tv_nsec < 0) {
        --times[1].tv_sec;
        times[1].tv_nsec += Billion;
    }
    return ::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0;
}

bool hasSpace(const fs::path& path, std::uint64_t bytes)
{
    std::error_code error;
    const auto value = fs::space(path, error);
    return !error && static_cast<__uint128_t>(value.available) >=
                         static_cast<__uint128_t>(bytes);
}

bool writeAll(int fd, const std::uint8_t* bytes, std::size_t size)
{
    while (size != 0) {
        const auto count = ::write(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace

MacFileClipboard::MacFileClipboard(PlankTransportNativeEndpoint* endpoint,
                                   std::string mode,
                                   ReadyCallback readyCallback,
                                   PublishCallback publishCallback)
    : m_Endpoint(endpoint),
      m_Mode(std::move(mode)),
      m_ReadyCallback(std::move(readyCallback)),
      m_PublishCallback(std::move(publishCallback))
{
}

MacFileClipboard::~MacFileClipboard()
{
    stop();
}

bool MacFileClipboard::clientToHostAllowed() const
{
    return m_Mode == "client-to-host" || m_Mode == "bidirectional";
}

bool MacFileClipboard::hostToClientAllowed() const
{
    return m_Mode == "host-to-client" || m_Mode == "bidirectional";
}

void MacFileClipboard::start()
{
    if ((!clientToHostAllowed() && !hostToClientAllowed()) || m_Endpoint == nullptr) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Running) return;
    m_Stop.store(false, std::memory_order_release);
    m_Running = true;
    m_Worker = std::thread(&MacFileClipboard::workerLoop, this);
}

void MacFileClipboard::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running) return;
        m_Stop.store(true, std::memory_order_release);
        m_PasteRequested = false;
        m_HostPublishPending = false;
    }
    m_Changed.notify_all();
    if (m_Worker.joinable()) m_Worker.join();
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Running = false;
    m_Current = {};
    m_Requested = {};
    m_PendingHostPaths.clear();
    m_HostPublishComplete = false;
    m_HostPublishSuccess = false;
}

void MacFileClipboard::pollLocalClipboardOnMainThread()
{
    SDL_assert([NSThread isMainThread]);
    if (!clientToHostAllowed()) return;
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        if (pasteboard == nil) return;
        const auto changeCount = static_cast<std::int64_t>(pasteboard.changeCount);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_Running || changeCount == m_Current.changeCount) return;
        }
        NSDictionary* options = @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};
        NSArray* values = [pasteboard readObjectsForClasses:@[[NSURL class]]
                                                    options:options];
        PasteboardSnapshot snapshot;
        snapshot.changeCount = changeCount;
        if (values.count <= PLANK_CLIPBOARD_FILE_MAX_TOP_LEVEL) {
            for (NSURL* url in values) {
                if (![url isFileURL] || url.fileSystemRepresentation == nullptr) {
                    snapshot.paths.clear();
                    break;
                }
                snapshot.paths.emplace_back(url.fileSystemRepresentation);
            }
        }
        if (pasteboard.changeCount != changeCount) return;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Running) m_Current = std::move(snapshot);
    }
}

bool MacFileClipboard::beginPasteOnMainThread()
{
    SDL_assert([NSThread isMainThread]);
    pollLocalClipboardOnMainThread();
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Running || m_Current.paths.empty()) return false;
    if (!m_PasteRequested) {
        m_Requested = m_Current;
        m_PasteRequested = true;
        m_Changed.notify_one();
    }
    return true;
}

bool MacFileClipboard::publishPendingHostFilesOnMainThread()
{
    SDL_assert([NSThread isMainThread]);
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running || !m_HostPublishPending) return false;
        paths = m_PendingHostPaths;
    }

    bool success = false;
    @autoreleasepool {
        NSMutableArray<NSURL*>* urls = [NSMutableArray arrayWithCapacity:paths.size()];
        for (const auto& path : paths) {
            NSString* value = [[NSString alloc] initWithBytes:path.data()
                                                       length:path.size()
                                                     encoding:NSUTF8StringEncoding];
            if (value == nil) {
                [urls removeAllObjects];
                break;
            }
            [urls addObject:[NSURL fileURLWithPath:value]];
        }
        if (urls.count == paths.size() && urls.count != 0) {
            NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
            [pasteboard clearContents];
            success = [pasteboard writeObjects:urls];
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_HostPublishPending = false;
        m_HostPublishComplete = true;
        m_HostPublishSuccess = success;
    }
    m_Changed.notify_all();
    return success;
}

void MacFileClipboard::workerLoop()
{
    for (;;) {
        PasteboardSnapshot snapshot;
        bool pasteRequested = false;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Changed.wait_for(lock, 25ms, [&] {
                return m_Stop.load(std::memory_order_acquire) || m_PasteRequested;
            });
            if (m_Stop.load(std::memory_order_acquire)) return;
            pasteRequested = m_PasteRequested;
            if (pasteRequested) snapshot = m_Requested;
        }
        if (pasteRequested) {
            const bool ready = transferSnapshot(snapshot);
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_PasteRequested = false;
                m_Requested = {};
            }
            if (ready && !m_Stop.load(std::memory_order_acquire) && m_ReadyCallback) {
                m_ReadyCallback();
            }
            continue;
        }
        if (!hostToClientAllowed()) continue;

        std::vector<std::uint8_t> firstRecord(
                PLANK_CLIPBOARD_FILE_HEADER_BYTES +
                PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT);
        std::size_t size = 0;
        const auto result = plank_transport_native_file_control_receive(
                m_Endpoint, firstRecord.data(), firstRecord.size(), &size, 1);
        if (result == PLANK_TRANSPORT_TIMEOUT) continue;
        if (result != PLANK_TRANSPORT_OK) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "File clipboard control receive failed");
            return;
        }
        firstRecord.resize(size);
        if (!receiveHostOffer(std::move(firstRecord)) &&
                !m_Stop.load(std::memory_order_acquire)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Host file clipboard offer failed");
        }
    }
}

bool MacFileClipboard::transferSnapshot(const PasteboardSnapshot& snapshot)
{
    std::vector<SourceEntry> entries;
    std::uint64_t totalBytes = 0;
    for (const auto& path : snapshot.paths) {
        if (!appendTree(fs::u8path(path), 0, 1, entries, totalBytes)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "File clipboard source manifest was rejected");
            return false;
        }
    }
    if (entries.empty()) return false;

    std::array<std::uint8_t, 16> offer {};
    std::uint64_t epoch = 0;
    if (!secureRandom(offer.data(), offer.size()) ||
            !secureRandom(&epoch, sizeof(epoch)) || epoch == 0) return false;
    WireSession wire(m_Endpoint, offer, epoch, m_Stop);
    PlankClipboardFileOfferBegin begin {
        PLANK_CLIPBOARD_FILE_CLIENT_TO_HOST,
        static_cast<std::uint64_t>(snapshot.changeCount + 1),
        static_cast<std::uint32_t>(snapshot.paths.size()),
        static_cast<std::uint32_t>(entries.size()),
        totalBytes,
    };
    std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_OFFER_BEGIN_BYTES> beginPayload {};
    if (!plank_clipboard_file_offer_begin_encode(beginPayload.data(), &begin) ||
            !wire.sendControl(PLANK_CLIPBOARD_FILE_OFFER_BEGIN, 0,
                              beginPayload.data(), beginPayload.size())) return false;
    for (const auto& entry : entries) {
        std::array<std::uint8_t,
            PLANK_CLIPBOARD_FILE_OFFER_ENTRY_PREFIX_BYTES +
            PLANK_CLIPBOARD_FILE_MAX_NAME_BYTES> payload {};
        PlankClipboardFileOfferEntry encoded {
            entry.id, entry.parent, entry.kind, entry.mode, entry.size,
            entry.mtimeNs,
            reinterpret_cast<const std::uint8_t*>(entry.name.data()),
            static_cast<std::uint16_t>(entry.name.size()),
        };
        std::size_t payloadSize = 0;
        if (!plank_clipboard_file_offer_entry_encode(
                    payload.data(), payload.size(), &encoded, &payloadSize) ||
                !wire.sendControl(PLANK_CLIPBOARD_FILE_OFFER_ENTRY, 0,
                                  payload.data(), payloadSize)) return false;
    }
    if (!wire.sendControl(PLANK_CLIPBOARD_FILE_OFFER_END, 0, nullptr, 0)) return false;

    std::vector<std::uint8_t> storage;
    std::uint64_t materialization = 0;
    bool acknowledged = false;
    while (!m_Stop.load(std::memory_order_acquire) && !materialization) {
        const auto record = wire.receiveControl(storage, 100);
        if (!record) {
            if (wire.failed()) return false;
            continue;
        }
        if (record->type == PLANK_CLIPBOARD_FILE_OFFER_ACK) {
            PlankClipboardFileOfferAck ack {};
            if (acknowledged || !plank_clipboard_file_offer_ack_decode(
                    record->payload, record->payload_size, &ack)) return false;
            acknowledged = true;
        } else if (record->type == PLANK_CLIPBOARD_FILE_MATERIALIZE) {
            if (!acknowledged || record->flags != PLANK_CLIPBOARD_FILE_FLAG_ALL_ITEMS ||
                    !plank_clipboard_file_materialization_decode(
                        record->payload, record->payload_size, &materialization)) return false;
        } else if (record->type == PLANK_CLIPBOARD_FILE_CANCEL ||
                   record->type == PLANK_CLIPBOARD_FILE_ERROR) {
            return false;
        } else {
            return false;
        }
    }

    for (const auto& entry : entries) {
        if (entry.kind != PLANK_CLIPBOARD_FILE_REGULAR) continue;
        QCryptographicHash digest(QCryptographicHash::Sha256);
        std::uint64_t offset = 0;
        int fd = -1;
        if (entry.size != 0) {
            fd = ::open(entry.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat current {};
            if (fd < 0 || ::fstat(fd, &current) != 0 || !statStable(current, entry)) {
                if (fd >= 0) ::close(fd);
                return false;
            }
        }
        while (offset < entry.size) {
            const auto record = wire.receiveControl(storage, 100);
            if (!record) {
                if (wire.failed() || m_Stop.load(std::memory_order_acquire)) {
                    ::close(fd);
                    return false;
                }
                continue;
            }
            if (record->type != PLANK_CLIPBOARD_FILE_READ_REQUEST) {
                ::close(fd);
                return false;
            }
            PlankClipboardFileRange range {};
            if (!plank_clipboard_file_decode_range(
                    record->payload, record->payload_size, 0, &range) ||
                    range.materialization_id != materialization ||
                    range.entry_id != entry.id || range.offset != offset ||
                    range.size > entry.size - offset) {
                ::close(fd);
                return false;
            }
            std::vector<std::uint8_t> payload(
                    PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES + range.size);
            if (!plank_clipboard_file_range_header(payload.data(), materialization,
                    entry.id, offset, range.size)) {
                ::close(fd);
                return false;
            }
            const auto count = ::pread(fd,
                    payload.data() + PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES,
                    range.size, static_cast<off_t>(offset));
            if (count != static_cast<ssize_t>(range.size)) {
                ::close(fd);
                return false;
            }
            digest.addData(QByteArrayView(reinterpret_cast<const char*>(
                    payload.data() + PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES), range.size));
            if (!wire.sendData(payload.data(), payload.size())) {
                ::close(fd);
                return false;
            }
            offset += range.size;
        }
        if (fd >= 0) {
            struct stat current {};
            const bool stable = ::fstat(fd, &current) == 0 && statStable(current, entry);
            ::close(fd);
            if (!stable) return false;
        }
        const QByteArray hash = digest.result();
        PlankClipboardFileEntryComplete complete {
            materialization, entry.id, entry.size, {0}
        };
        std::memcpy(complete.sha256, hash.constData(), sizeof(complete.sha256));
        std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE_BYTES> payload {};
        if (!plank_clipboard_file_entry_complete_encode(payload.data(), &complete) ||
                !wire.sendControl(PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE, 0,
                                  payload.data(), payload.size())) return false;
    }

    while (!m_Stop.load(std::memory_order_acquire)) {
        const auto record = wire.receiveControl(storage, 100);
        if (!record) {
            if (wire.failed()) return false;
            continue;
        }
        std::uint64_t ready = 0;
        if (record->type == PLANK_CLIPBOARD_FILE_TRANSFER_READY &&
                plank_clipboard_file_materialization_decode(
                    record->payload, record->payload_size, &ready) &&
                ready == materialization) {
            return true;
        }
        if (record->type == PLANK_CLIPBOARD_FILE_CANCEL ||
                record->type == PLANK_CLIPBOARD_FILE_ERROR) return false;
        return false;
    }
    return false;
}

bool MacFileClipboard::receiveHostOffer(std::vector<std::uint8_t> firstRecord)
{
    PlankClipboardFileRecord first {};
    if (!plank_clipboard_file_decode(firstRecord.data(), firstRecord.size(),
            PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &first) ||
            first.type != PLANK_CLIPBOARD_FILE_OFFER_BEGIN || first.sequence != 1) {
        return false;
    }
    PlankClipboardFileOfferBegin begin {};
    if (!plank_clipboard_file_offer_begin_decode(
            first.payload, first.payload_size, &begin) ||
            begin.direction != PLANK_CLIPBOARD_FILE_HOST_TO_CLIENT ||
            begin.total_bytes > (1ULL << 40)) {
        return false;
    }

    std::array<std::uint8_t, 16> offer {};
    std::memcpy(offer.data(), first.offer_id, offer.size());
    WireSession wire(m_Endpoint, offer, first.session_epoch, m_Stop, 2);
    std::uint64_t materialization = 0;
    const auto sendError = [&](std::uint16_t reason) {
        PlankClipboardFileTerminal terminal {materialization, reason, 0};
        std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_TERMINAL_BYTES> payload {};
        return plank_clipboard_file_terminal_encode(payload.data(), &terminal) &&
                wire.sendControl(PLANK_CLIPBOARD_FILE_ERROR, 0,
                                 payload.data(), payload.size());
    };

    std::vector<SinkEntry> entries;
    entries.reserve(begin.entry_count);
    std::map<std::uint64_t, std::size_t> byId;
    std::set<std::string> exactPaths;
    std::set<std::string> foldedPaths;
    std::uint64_t totalBytes = 0;
    std::uint32_t topLevelCount = 0;
    std::size_t manifestBytes = first.payload_size;
    std::vector<std::uint8_t> storage;
    bool valid = true;
    for (std::uint32_t index = 0; valid && index < begin.entry_count; ++index) {
        std::optional<PlankClipboardFileRecord> record;
        while (!record && !m_Stop.load(std::memory_order_acquire)) {
            record = wire.receiveControl(storage, 100);
            if (!record && wire.failed()) valid = false;
        }
        PlankClipboardFileOfferEntry decoded {};
        if (!valid || !record || record->type != PLANK_CLIPBOARD_FILE_OFFER_ENTRY ||
                !plank_clipboard_file_offer_entry_decode(
                    record->payload, record->payload_size, &decoded) ||
                manifestBytes > PLANK_CLIPBOARD_FILE_MAX_MANIFEST_BYTES -
                                    record->payload_size ||
                byId.find(decoded.entry_id) != byId.end()) {
            valid = false;
            break;
        }
        manifestBytes += record->payload_size;
        SinkEntry entry;
        entry.id = decoded.entry_id;
        entry.parent = decoded.parent_id;
        entry.kind = decoded.kind;
        entry.mode = decoded.mode;
        entry.size = decoded.size;
        entry.mtimeNs = decoded.mtime_ns;
        entry.name.assign(reinterpret_cast<const char*>(decoded.name), decoded.name_size);
        if (entry.parent == 0) {
            entry.depth = 1;
            entry.relative = fs::u8path(entry.name);
            ++topLevelCount;
        } else {
            const auto parent = byId.find(entry.parent);
            if (parent == byId.end() ||
                    entries[parent->second].kind != PLANK_CLIPBOARD_FILE_DIRECTORY) {
                valid = false;
                break;
            }
            entry.depth = entries[parent->second].depth + 1;
            entry.relative = entries[parent->second].relative / fs::u8path(entry.name);
        }
        const auto relative = entry.relative.generic_u8string();
        if (entry.depth > PLANK_CLIPBOARD_FILE_MAX_DEPTH ||
                !exactPaths.insert(relative).second ||
                !foldedPaths.insert(asciiFold(relative)).second ||
                entry.size > std::numeric_limits<std::uint64_t>::max() - totalBytes) {
            valid = false;
            break;
        }
        totalBytes += entry.size;
        byId.emplace(entry.id, entries.size());
        entries.push_back(std::move(entry));
    }

    std::optional<PlankClipboardFileRecord> end;
    while (valid && !end && !m_Stop.load(std::memory_order_acquire)) {
        end = wire.receiveControl(storage, 100);
        if (!end && wire.failed()) valid = false;
    }
    valid = valid && end && end->type == PLANK_CLIPBOARD_FILE_OFFER_END &&
            end->payload_size == 0 && entries.size() == begin.entry_count &&
            topLevelCount == begin.top_level_count && totalBytes == begin.total_bytes;
    if (!valid || m_Stop.load(std::memory_order_acquire)) {
        if (!m_Stop.load(std::memory_order_acquire)) {
            sendError(PLANK_CLIPBOARD_FILE_REASON_INVALID_MANIFEST);
        }
        return false;
    }

    const fs::path root = stagingRoot();
    const fs::path partial = root / (offerName(offer) + ".partial");
    const fs::path tree = partial / "tree";
    const fs::path blobs = partial / "blobs";
    const fs::path completed = root / offerName(offer);
    std::error_code error;
    fs::create_directories(root, error);
    if (!error && ::chmod(root.c_str(), 0700) != 0) error =
            std::error_code(errno, std::generic_category());
    if (!error && (fs::exists(partial) || fs::exists(completed))) error =
            std::make_error_code(std::errc::file_exists);
    if (!error) fs::create_directories(tree, error);
    if (!error) fs::create_directories(blobs, error);
    if (!error && (::chmod(partial.c_str(), 0700) != 0 ||
                   ::chmod(tree.c_str(), 0700) != 0 ||
                   ::chmod(blobs.c_str(), 0700) != 0)) {
        error = std::error_code(errno, std::generic_category());
    }
    if (!error && !hasSpace(root, totalBytes)) {
        fs::remove_all(partial, error);
        sendError(PLANK_CLIPBOARD_FILE_REASON_NO_SPACE);
        return false;
    }
    if (error) {
        fs::remove_all(partial, error);
        sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
        return false;
    }
    const auto cleanup = [&] {
        std::error_code ignored;
        fs::remove_all(partial, ignored);
    };

    for (const auto& entry : entries) {
        if (entry.kind != PLANK_CLIPBOARD_FILE_DIRECTORY) continue;
        fs::create_directory(tree / entry.relative, error);
        if (error) {
            cleanup();
            sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
            return false;
        }
    }

    std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_OFFER_ACK_BYTES> ack {};
    if (!plank_clipboard_file_offer_ack_encode(
                ack.data(), PLANK_CLIPBOARD_FILE_DATA_BYTES,
                PLANK_CLIPBOARD_FILE_MAX_OUTSTANDING_BYTES) ||
            !wire.sendControl(PLANK_CLIPBOARD_FILE_OFFER_ACK, 0,
                              ack.data(), ack.size()) ||
            !secureRandom(&materialization, sizeof(materialization)) ||
            materialization == 0) {
        cleanup();
        return false;
    }
    std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_MATERIALIZE_BYTES> materialize {};
    if (!plank_clipboard_file_materialization_encode(
                materialize.data(), materialization) ||
            !wire.sendControl(PLANK_CLIPBOARD_FILE_MATERIALIZE,
                              PLANK_CLIPBOARD_FILE_FLAG_ALL_ITEMS,
                              materialize.data(), materialize.size())) {
        cleanup();
        return false;
    }

    for (const auto& entry : entries) {
        if (entry.kind != PLANK_CLIPBOARD_FILE_REGULAR) continue;
        const fs::path blob = blobs / std::to_string(entry.id);
        const int fd = ::open(blob.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            cleanup();
            sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
            return false;
        }
        QCryptographicHash digest(QCryptographicHash::Sha256);
        std::uint64_t offset = 0;
        bool fileValid = true;
        while (fileValid && offset < entry.size &&
                !m_Stop.load(std::memory_order_acquire)) {
            const auto amount = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    PLANK_CLIPBOARD_FILE_DATA_BYTES, entry.size - offset));
            std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES> request {};
            fileValid = plank_clipboard_file_range_header(
                    request.data(), materialization, entry.id, offset, amount) &&
                    wire.sendControl(PLANK_CLIPBOARD_FILE_READ_REQUEST, 0,
                                     request.data(), request.size());
            std::optional<PlankClipboardFileRecord> record;
            while (fileValid && !record && !m_Stop.load(std::memory_order_acquire)) {
                record = wire.receiveData(storage, 100);
                if (!record && wire.failed()) fileValid = false;
            }
            PlankClipboardFileRange range {};
            fileValid = fileValid && record && record->type == PLANK_CLIPBOARD_FILE_DATA &&
                    plank_clipboard_file_decode_range(
                        record->payload, record->payload_size, 1, &range) &&
                    range.materialization_id == materialization &&
                    range.entry_id == entry.id && range.offset == offset &&
                    range.size == amount && writeAll(fd, range.bytes, range.size);
            if (fileValid) {
                digest.addData(QByteArrayView(
                        reinterpret_cast<const char*>(range.bytes), range.size));
                offset += range.size;
            }
        }

        std::optional<PlankClipboardFileRecord> completeRecord;
        while (fileValid && !completeRecord &&
                !m_Stop.load(std::memory_order_acquire)) {
            completeRecord = wire.receiveControl(storage, 100);
            if (!completeRecord && wire.failed()) fileValid = false;
        }
        PlankClipboardFileEntryComplete complete {};
        const QByteArray hash = digest.result();
        fileValid = fileValid && completeRecord &&
                completeRecord->type == PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE &&
                plank_clipboard_file_entry_complete_decode(
                    completeRecord->payload, completeRecord->payload_size, &complete) &&
                complete.materialization_id == materialization &&
                complete.entry_id == entry.id && complete.size == entry.size &&
                hash.size() == static_cast<int>(sizeof(complete.sha256)) &&
                std::memcmp(complete.sha256, hash.constData(),
                            sizeof(complete.sha256)) == 0 &&
                ::fsync(fd) == 0 && ::fchmod(fd, entry.mode & 0700) == 0 &&
                setFileMtime(fd, entry.mtimeNs);
        ::close(fd);
        if (fileValid) fs::rename(blob, tree / entry.relative, error);
        if (!fileValid || error) {
            cleanup();
            if (!m_Stop.load(std::memory_order_acquire)) {
                sendError(fileValid ?
                        PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE :
                        PLANK_CLIPBOARD_FILE_REASON_INTEGRITY_FAILED);
            }
            return false;
        }
    }

    for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
        if (iterator->kind != PLANK_CLIPBOARD_FILE_DIRECTORY) continue;
        const auto path = tree / iterator->relative;
        if (::chmod(path.c_str(), iterator->mode & 0700) != 0 ||
                !setPathMtime(path, iterator->mtimeNs)) {
            cleanup();
            sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
            return false;
        }
    }
    fs::remove(blobs, error);
    if (!error) fs::rename(tree, completed, error);
    if (error) {
        cleanup();
        sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
        return false;
    }
    fs::remove(partial, error);

    std::vector<std::string> paths;
    for (const auto& entry : entries) {
        if (entry.parent == 0) paths.push_back((completed / entry.relative).u8string());
    }
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_PendingHostPaths = std::move(paths);
        m_HostPublishPending = true;
        m_HostPublishComplete = false;
        m_HostPublishSuccess = false;
    }
    if (!m_PublishCallback || !m_PublishCallback()) {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_HostPublishPending = false;
            m_PendingHostPaths.clear();
        }
        sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
        return false;
    }
    bool published = false;
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_Changed.wait(lock, [&] {
            return m_Stop.load(std::memory_order_acquire) || m_HostPublishComplete;
        });
        published = m_HostPublishComplete && m_HostPublishSuccess;
        m_HostPublishComplete = false;
        m_HostPublishSuccess = false;
        m_PendingHostPaths.clear();
    }
    if (!published) {
        if (!m_Stop.load(std::memory_order_acquire)) {
            sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
        }
        return false;
    }
    return wire.sendControl(PLANK_CLIPBOARD_FILE_TRANSFER_READY, 0,
                            materialize.data(), materialize.size());
}
