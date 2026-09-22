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
#include <optional>
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
                const std::atomic_bool& stop)
        : m_Endpoint(endpoint), m_Offer(offer), m_Epoch(epoch), m_Stop(stop) {}

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
        storage.resize(PLANK_CLIPBOARD_FILE_HEADER_BYTES +
                       PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT);
        std::size_t size = 0;
        const auto result = plank_transport_native_file_control_receive(
                m_Endpoint, storage.data(), storage.size(), &size, timeoutMs);
        if (result == PLANK_TRANSPORT_TIMEOUT) return std::nullopt;
        if (result != PLANK_TRANSPORT_OK) {
            m_Failed = true;
            return std::nullopt;
        }
        storage.resize(size);
        PlankClipboardFileRecord record {};
        if (!plank_clipboard_file_decode(storage.data(), storage.size(),
                PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &record) ||
                std::memcmp(record.offer_id, m_Offer.data(), m_Offer.size()) != 0 ||
                record.session_epoch != m_Epoch ||
                record.sequence != m_ReceiveSequence++) {
            m_Failed = true;
            return std::nullopt;
        }
        return record;
    }

    bool failed() const { return m_Failed; }

private:
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

} // namespace

MacFileClipboard::MacFileClipboard(PlankTransportNativeEndpoint* endpoint,
                                   std::string mode,
                                   ReadyCallback readyCallback)
    : m_Endpoint(endpoint),
      m_Mode(std::move(mode)),
      m_ReadyCallback(std::move(readyCallback))
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

void MacFileClipboard::start()
{
    if (!clientToHostAllowed() || m_Endpoint == nullptr) return;
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
    }
    m_Changed.notify_all();
    if (m_Worker.joinable()) m_Worker.join();
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Running = false;
    m_Current = {};
    m_Requested = {};
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

void MacFileClipboard::workerLoop()
{
    for (;;) {
        PasteboardSnapshot snapshot;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Changed.wait(lock, [&] {
                return m_Stop.load(std::memory_order_acquire) || m_PasteRequested;
            });
            if (m_Stop.load(std::memory_order_acquire)) return;
            snapshot = m_Requested;
        }
        const bool ready = transferSnapshot(snapshot);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_PasteRequested = false;
            m_Requested = {};
        }
        if (ready && !m_Stop.load(std::memory_order_acquire) && m_ReadyCallback) {
            m_ReadyCallback();
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
