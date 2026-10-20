#include "windowsfileclipboard.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include "windowsfileclipboardpaths.h"
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QDebug>

#include <plank_clipboard_file_wire.h>
#include <plank_transport.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>



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
    BY_HANDLE_FILE_INFORMATION identity {};
    std::shared_ptr<void> handle;
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

// FILE_SHARE_READ only freezes each manifest entry against writes and renames.
// OPEN_REPARSE_POINT prevents following links, including directory junctions.
using NativeHandle = std::shared_ptr<void>;
NativeHandle openSource(const fs::path& path, std::map<fs::path, NativeHandle>& ancestors)
{
    if (!plank::clipboard::windows::plainAbsolutePath(path)) return {};
    // Lock ancestors in root-to-leaf order before opening the selected entry.
    // Keep one handle per directory across the whole manifest.
    fs::path current = path.root_path();
    for (const auto& component : path.parent_path().relative_path()) {
        current /= component;
        if (ancestors.find(current) != ancestors.end()) continue;
        HANDLE ancestor = CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        BY_HANDLE_FILE_INFORMATION info {};
        if (ancestor == INVALID_HANDLE_VALUE ||
                !GetFileInformationByHandle(ancestor, &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (ancestor != INVALID_HANDLE_VALUE) CloseHandle(ancestor);
            return {};
        }
        ancestors.emplace(current, NativeHandle(ancestor, [](void* value) { CloseHandle(value); }));
    }
    HANDLE value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (value == INVALID_HANDLE_VALUE) return {};
    return NativeHandle(value, [](void* handle) { CloseHandle(handle); });
}

std::int64_t filetimeNs(const FILETIME& time)
{
    ULARGE_INTEGER ticks {};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t Epoch = 116444736000000000ULL;
    if (ticks.QuadPart < Epoch) return 0;
    const auto value = ticks.QuadPart - Epoch;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 100)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value * 100);
}

bool statStable(HANDLE handle, const SourceEntry& entry)
{
    BY_HANDLE_FILE_INFORMATION current {};
    return GetFileInformationByHandle(handle, &current) &&
            current.dwVolumeSerialNumber == entry.identity.dwVolumeSerialNumber &&
            current.nFileIndexHigh == entry.identity.nFileIndexHigh &&
            current.nFileIndexLow == entry.identity.nFileIndexLow &&
            current.nFileSizeHigh == entry.identity.nFileSizeHigh &&
            current.nFileSizeLow == entry.identity.nFileSizeLow &&
            CompareFileTime(&current.ftLastWriteTime, &entry.identity.ftLastWriteTime) == 0 &&
            !(current.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool appendTree(const fs::path& path, std::uint64_t parent, unsigned depth,
                std::vector<SourceEntry>& entries, std::uint64_t& totalBytes,
                std::map<fs::path, NativeHandle>& ancestors)
{
    if (depth > PLANK_CLIPBOARD_FILE_MAX_DEPTH ||
            entries.size() >= PLANK_CLIPBOARD_FILE_MAX_ENTRIES) return false;
    auto handle = openSource(path, ancestors);
    BY_HANDLE_FILE_INFORMATION value {};
    if (!handle || !GetFileInformationByHandle(handle.get(), &value) ||
            (value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            GetFileType(handle.get()) != FILE_TYPE_DISK) return false;
    const bool directory = (value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!directory && value.nNumberOfLinks != 1) return false;
    const std::string name = path.filename().u8string();
    if (!plank::clipboard::windows::validName(name)) return false;
    SourceEntry entry;
    entry.id = entries.size() + 1;
    entry.parent = parent;
    entry.kind = directory ? PLANK_CLIPBOARD_FILE_DIRECTORY : PLANK_CLIPBOARD_FILE_REGULAR;
    entry.mode = directory ? 0700 : 0600;
    entry.size = directory ? 0 : (static_cast<std::uint64_t>(value.nFileSizeHigh) << 32) |
                                value.nFileSizeLow;
    entry.mtimeNs = filetimeNs(value.ftLastWriteTime);
    entry.identity = value;
    entry.handle = std::move(handle);
    entry.name = name;
    entry.path = path;
    if (entry.size > std::numeric_limits<std::uint64_t>::max() - totalBytes) return false;
    totalBytes += entry.size;
    const auto entryId = entry.id;
    entries.push_back(std::move(entry));
    if (directory) {
        std::error_code error;
        std::vector<fs::path> children;
        for (fs::directory_iterator iterator(path, error), end;
             !error && iterator != end; iterator.increment(error)) children.push_back(iterator->path());
        if (error) return false;
        std::sort(children.begin(), children.end());
        for (const auto& child : children)
            if (!appendTree(child, entryId, depth + 1, entries, totalBytes, ancestors)) return false;
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

    void allowHostPreemption(bool enabled) { m_AllowHostPreemption = enabled; }
    bool hasForeignOffer() const { return !m_ForeignOffer.empty(); }
    std::vector<std::uint8_t> takeForeignOffer() { return std::move(m_ForeignOffer); }

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
        int32_t result;
        if (!content && !m_DeferredControl.empty()) {
            storage = std::move(m_DeferredControl);
            m_DeferredControl.clear();
            size = storage.size();
            result = PLANK_TRANSPORT_OK;
        } else {
            result = content ?
                    plank_transport_native_file_data_receive(
                        m_Endpoint, storage.data(), storage.size(), &size, timeoutMs) :
                    plank_transport_native_file_control_receive(
                        m_Endpoint, storage.data(), storage.size(), &size, timeoutMs);
        }
        if (content && result == PLANK_TRANSPORT_TIMEOUT && m_DeferredControl.empty()) {
            std::vector<std::uint8_t> control(PLANK_CLIPBOARD_FILE_HEADER_BYTES + PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT);
            std::size_t count = 0;
            const auto controlResult = plank_transport_native_file_control_receive(
                m_Endpoint, control.data(), control.size(), &count, 1);
            if (controlResult == PLANK_TRANSPORT_OK) {
                control.resize(count);
                PlankClipboardFileRecord terminal {};
                if (plank_clipboard_file_decode(control.data(), control.size(),
                        PLANK_CLIPBOARD_FILE_CONTROL_PAYLOAD_LIMIT, &terminal) &&
                        (terminal.type == PLANK_CLIPBOARD_FILE_CANCEL || terminal.type == PLANK_CLIPBOARD_FILE_ERROR) &&
                        terminal.sequence == m_ReceiveSequence && terminal.session_epoch == m_Epoch &&
                        std::memcmp(terminal.offer_id, m_Offer.data(), m_Offer.size()) == 0) {
                    storage = std::move(control);
                    size = storage.size();
                    result = PLANK_TRANSPORT_OK;
                } else m_DeferredControl = std::move(control);
            } else if (controlResult != PLANK_TRANSPORT_TIMEOUT) {
                m_Failed = true;
                return std::nullopt;
            }
        }
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            if (std::chrono::steady_clock::now() - m_LastActivity > 60s) m_Failed = true;
            return std::nullopt;
        }
        m_LastActivity = std::chrono::steady_clock::now();
        if (result != PLANK_TRANSPORT_OK) {
            m_Failed = true;
            return std::nullopt;
        }
        storage.resize(size);
        PlankClipboardFileRecord record {};
        if (!plank_clipboard_file_decode(storage.data(), storage.size(),
                static_cast<std::uint32_t>(payloadLimit), &record)) {
            m_Failed = true;
            return std::nullopt;
        }
        if (!content && m_AllowHostPreemption && record.type == PLANK_CLIPBOARD_FILE_OFFER_BEGIN &&
                record.sequence == 1 &&
                (std::memcmp(record.offer_id, m_Offer.data(), m_Offer.size()) != 0 || record.session_epoch != m_Epoch)) {
            PlankClipboardFileOfferBegin begin {};
            if (plank_clipboard_file_offer_begin_decode(record.payload, record.payload_size, &begin) &&
                    begin.direction == PLANK_CLIPBOARD_FILE_HOST_TO_CLIENT) {
                m_ForeignOffer = storage;
                return std::nullopt; // Host source wins; no Client data is in flight yet.
            }
        }
        if (std::memcmp(record.offer_id, m_Offer.data(), m_Offer.size()) != 0 ||
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
            if (result == PLANK_TRANSPORT_OK) {
                m_LastActivity = std::chrono::steady_clock::now();
                return true;
            }
            if (std::chrono::steady_clock::now() - m_LastActivity > 60s) return false;
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
    bool m_AllowHostPreemption = false;
    std::vector<std::uint8_t> m_ForeignOffer;
    std::vector<std::uint8_t> m_DeferredControl;
    std::chrono::steady_clock::time_point m_LastActivity = std::chrono::steady_clock::now();
};

bool secureRandom(void* bytes, std::size_t size)
{
    auto* output = static_cast<std::uint8_t*>(bytes);
    while (size != 0) {
        const auto random = QRandomGenerator::system()->generate();
        const auto amount = std::min(size, sizeof(random));
        std::memcpy(output, &random, amount);
        output += amount;
        size -= amount;
    }
    return true;
}

// New directories receive a protected DACL for this user and SYSTEM. Never
// repair or adopt pre-existing directories, which could belong to another app.
bool createPrivateDirectory(const fs::path& path)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> storage(size);
    const bool tokenOk = GetTokenInformation(token, TokenUser, storage.data(), size, &size);
    CloseHandle(token);
    if (!tokenOk) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid))
        return false;
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
    const bool created = CreateDirectoryW(path.c_str(), &attributes);
    LocalFree(descriptor);
    return created;
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

bool setFileMtime(HANDLE handle, std::int64_t nanoseconds)
{
    if (nanoseconds < 0) return true; // Windows cannot represent every Unix timestamp.
    ULARGE_INTEGER ticks {};
    ticks.QuadPart = static_cast<std::uint64_t>(nanoseconds / 100) + 116444736000000000ULL;
    FILETIME time {ticks.LowPart, ticks.HighPart};
    return SetFileTime(handle, nullptr, nullptr, &time);
}

bool setPathMtime(const fs::path& path, std::int64_t nanoseconds)
{
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool success = setFileMtime(handle, nanoseconds);
    CloseHandle(handle);
    return success;
}

bool hasSpace(const fs::path& path, std::uint64_t bytes)
{
    std::error_code error;
    const auto value = fs::space(path, error);
    return !error && value.available >= bytes;
}

bool writeAll(HANDLE handle, const std::uint8_t* bytes, std::size_t size)
{
    while (size != 0) {
        DWORD count = 0;
        if (!WriteFile(handle, bytes, static_cast<DWORD>(size), &count, nullptr) || count == 0)
            return false;
        bytes += count;
        size -= count;
    }
    return true;
}

} // namespace

WindowsFileClipboard::WindowsFileClipboard(PlankTransportNativeEndpoint* endpoint,
                                   std::string mode,
                                   ReadyCallback readyCallback,
                                   PublishCallback publishCallback,
                                   FocusPredicate focusPredicate)
    : m_Endpoint(endpoint),
      m_Mode(std::move(mode)),
      m_ReadyCallback(std::move(readyCallback)),
      m_PublishCallback(std::move(publishCallback)),
      m_FocusPredicate(std::move(focusPredicate))
{
}

WindowsFileClipboard::~WindowsFileClipboard()
{
    stop();
}

bool WindowsFileClipboard::clientToHostAllowed() const
{
    return m_Mode == "client-to-host" || m_Mode == "bidirectional";
}

bool WindowsFileClipboard::hostToClientAllowed() const
{
    return m_Mode == "host-to-client" || m_Mode == "bidirectional";
}

void WindowsFileClipboard::start()
{
    if ((!clientToHostAllowed() && !hostToClientAllowed()) || m_Endpoint == nullptr) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Running) return;
    m_Stop.store(false, std::memory_order_release);
    m_Running = true;
    m_Worker = std::thread(&WindowsFileClipboard::workerLoop, this);
}

void WindowsFileClipboard::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running) return;
        m_Stop.store(true, std::memory_order_release);
        m_PasteRequested = false;
        ++m_PasteEpoch;
        m_ReadyPaste = false;
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
    if (m_PublishedSequence && GetClipboardSequenceNumber() == m_PublishedSequence &&
            OpenClipboard(nullptr)) {
        if (GetClipboardSequenceNumber() == m_PublishedSequence) EmptyClipboard();
        CloseClipboard();
    }
    m_PublishedSequence = 0;
    if (!m_StagingRoot.empty()) {
        std::error_code ignored;
        fs::remove_all(fs::u8path(m_StagingRoot), ignored);
        m_StagingRoot.clear();
    }
}

void WindowsFileClipboard::pollLocalClipboardOnMainThread()
{
    if (!clientToHostAllowed()) return;
    const auto sequence = GetClipboardSequenceNumber();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running || sequence == m_Current.changeCount) return;
    }
    if (!OpenClipboard(nullptr)) return;
    PasteboardSnapshot snapshot;
    snapshot.changeCount = sequence;
    if (sequence != m_PublishedSequence && IsClipboardFormatAvailable(CF_HDROP)) {
        const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        const UINT count = drop ? DragQueryFileW(drop, 0xffffffff, nullptr, 0) : 0;
        if (count <= PLANK_CLIPBOARD_FILE_MAX_TOP_LEVEL) {
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                if (!length || length > 32767) { snapshot.paths.clear(); break; }
                std::vector<wchar_t> path(length + 1);
                if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
                    snapshot.paths.clear(); break;
                }
                try { snapshot.paths.push_back(fs::path(path.data()).u8string()); }
                catch (const std::exception&) { snapshot.paths.clear(); break; }
            }
        }
    }
    const bool unchanged = GetClipboardSequenceNumber() == sequence;
    CloseClipboard();
    if (!unchanged) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Running) m_Current = std::move(snapshot);
}

bool WindowsFileClipboard::beginPasteOnMainThread()
{
    pollLocalClipboardOnMainThread();
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Running || m_Current.paths.empty() ||
            m_Current.changeCount != GetClipboardSequenceNumber()) return false;
    if (!m_PasteRequested || m_Requested.pasteEpoch != m_PasteEpoch) {
        m_Requested = m_Current;
        m_Requested.pasteEpoch = m_PasteEpoch;
        m_PasteRequested = true;
        m_Changed.notify_one();
    }
    return true;
}

void WindowsFileClipboard::cancelPendingPasteOnMainThread()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ++m_PasteEpoch;
    m_ReadyPaste = false;
}

bool WindowsFileClipboard::takeReadyPasteOnMainThread()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    const bool ready = m_Running && m_ReadyPaste;
    m_ReadyPaste = false;
    return ready;
}

bool WindowsFileClipboard::publishPendingHostFilesOnMainThread()
{
    std::vector<std::string> paths;
    std::uint32_t expectedSequence = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Running || !m_HostPublishPending) return false;
        paths = m_PendingHostPaths;
        expectedSequence = m_HostClipboardSequence;
    }

    const bool mayPublish = !m_FocusPredicate || m_FocusPredicate();
    struct ClipboardOwner {
        HWND window = CreateWindowExW(0, L"STATIC", L"Fernweh file clipboard", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~ClipboardOwner() { if (window) DestroyWindow(window); }
    } owner;
    bool success = false, busy = false;
    std::vector<wchar_t> names;
    for (const auto& path : paths) {
        const auto native = fs::u8path(path).wstring();
        names.insert(names.end(), native.begin(), native.end());
        names.push_back(L'\0');
    }
    names.push_back(L'\0');
    const auto bytes = sizeof(DROPFILES) + names.size() * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (mayPublish && memory && !paths.empty()) {
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop) {
            drop->pFiles = sizeof(DROPFILES);
            drop->fWide = TRUE;
            std::memcpy(reinterpret_cast<std::uint8_t*>(drop) + sizeof(DROPFILES),
                        names.data(), names.size() * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (owner.window && OpenClipboard(owner.window)) {
                // Check after acquiring the clipboard: a local Copy made during
                // network IO or while OpenClipboard was blocked always wins.
                success = GetClipboardSequenceNumber() == expectedSequence &&
                        (!m_FocusPredicate || m_FocusPredicate()) &&
                        EmptyClipboard() && SetClipboardData(CF_HDROP, memory) != nullptr;
                if (success) {
                    memory = nullptr; // clipboard now owns this allocation
                    m_PublishedSequence = GetClipboardSequenceNumber();
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    m_Current = {};
                    m_Current.changeCount = m_PublishedSequence.load();
                }
                CloseClipboard();
            } else if (owner.window && GetClipboardSequenceNumber() == expectedSequence) {
                busy = true;
            }
        }
    }
    if (memory) GlobalFree(memory);
    if (busy) return false; // Keep pending; the SDL clipboard poll retries.

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_HostPublishPending = false;
        m_HostPublishComplete = true;
        m_HostPublishSuccess = success;
    }
    m_Changed.notify_all();
    return success;
}

void WindowsFileClipboard::workerLoop()
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
            bool ready = false;
            try {
                for (unsigned attempt = 0; attempt < 3 && !m_Stop.load(std::memory_order_acquire); ++attempt) {
                    std::vector<std::uint8_t> foreignOffer;
                    const auto result = transferSnapshot(snapshot, foreignOffer);
                    ready = result == SourceResult::Ready;
                    if (result != SourceResult::Yielded) break;
                    if (!receiveHostOffer(std::move(foreignOffer))) break;
                    std::lock_guard<std::mutex> lock(m_Mutex);
                    if (snapshot.pasteEpoch != m_PasteEpoch) break;
                }
            }
            catch (const std::exception&) {
                qWarning("File clipboard source is unavailable");
            }
            {
                std::lock_guard<std::mutex> lock(m_Mutex);
                if (m_Requested.pasteEpoch == snapshot.pasteEpoch) {
                    m_PasteRequested = false;
                    m_Requested = {};
                }
                ready = ready && snapshot.pasteEpoch == m_PasteEpoch;
                m_ReadyPaste = ready;
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
            qWarning("File clipboard control receive failed");
            return;
        }
        firstRecord.resize(size);
        bool received = false;
        try { received = receiveHostOffer(std::move(firstRecord)); }
        catch (const std::exception&) {
            qWarning("File clipboard destination is unavailable");
        }
        if (!received && !m_Stop.load(std::memory_order_acquire)) {
            qWarning("Host file clipboard offer failed");
        }
    }
}

WindowsFileClipboard::SourceResult WindowsFileClipboard::transferSnapshot(
        const PasteboardSnapshot& snapshot, std::vector<std::uint8_t>& yieldedOffer)
{
    const auto cancelled = [&] {
        if (m_Stop.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(m_Mutex);
        return snapshot.pasteEpoch != m_PasteEpoch;
    };
    if (cancelled()) return SourceResult::Failed;
    std::map<fs::path, NativeHandle> ancestors;
    std::vector<SourceEntry> entries;
    std::uint64_t totalBytes = 0;
    for (const auto& path : snapshot.paths) {
        if (!appendTree(fs::u8path(path), 0, 1, entries, totalBytes, ancestors)) {
            qWarning("File clipboard source manifest was rejected");
            return SourceResult::Failed;
        }
    }
    if (entries.empty() || totalBytes > (1ULL << 40)) return SourceResult::Failed;
    std::size_t manifestBytes = PLANK_CLIPBOARD_FILE_OFFER_BEGIN_BYTES;
    std::set<std::pair<std::uint64_t, QString>> siblingNames;
    for (const auto& entry : entries) {
        if (!siblingNames.emplace(entry.parent,
                plank::clipboard::windows::foldedPath(entry.name)).second) return SourceResult::Failed;
        const auto amount = PLANK_CLIPBOARD_FILE_OFFER_ENTRY_PREFIX_BYTES + entry.name.size();
        if (amount > PLANK_CLIPBOARD_FILE_MAX_MANIFEST_BYTES - manifestBytes) return SourceResult::Failed;
        manifestBytes += amount;
        if (!statStable(entry.handle.get(), entry)) return SourceResult::Failed;
    }

    std::array<std::uint8_t, 16> offer {};
    std::uint64_t epoch = 0;
    if (!secureRandom(offer.data(), offer.size()) ||
            !secureRandom(&epoch, sizeof(epoch)) || epoch == 0) return SourceResult::Failed;
    WireSession wire(m_Endpoint, offer, epoch, m_Stop);
    wire.allowHostPreemption(true);
    std::uint64_t materialization = 0;
    bool offered = false, complete = false, peerTerminated = false, yielded = false;
    struct SourceAbort {
        std::function<void()> abort;
        ~SourceAbort() { abort(); }
    } sourceAbort{[&] {
        if (!offered || complete || peerTerminated || yielded || m_Stop.load(std::memory_order_acquire)) return;
        PlankClipboardFileTerminal terminal {materialization, PLANK_CLIPBOARD_FILE_REASON_SOURCE_UNAVAILABLE, 0};
        std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_TERMINAL_BYTES> payload {};
        if (plank_clipboard_file_terminal_encode(payload.data(), &terminal))
            wire.sendControl(PLANK_CLIPBOARD_FILE_CANCEL, 0, payload.data(), payload.size());
    }};
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
                              beginPayload.data(), beginPayload.size())) return SourceResult::Failed;
    offered = true;
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
                                  payload.data(), payloadSize)) return SourceResult::Failed;
    }
    if (!wire.sendControl(PLANK_CLIPBOARD_FILE_OFFER_END, 0, nullptr, 0)) return SourceResult::Failed;

    std::vector<std::uint8_t> storage;
    bool acknowledged = false;
    while (!cancelled() && !materialization) {
        const auto record = wire.receiveControl(storage, 100);
        if (!record) {
            if (wire.hasForeignOffer()) {
                yielded = true;
                yieldedOffer = wire.takeForeignOffer();
                return SourceResult::Yielded;
            }
            if (wire.failed()) return SourceResult::Failed;
            continue;
        }
        if (record->type == PLANK_CLIPBOARD_FILE_OFFER_ACK) {
            PlankClipboardFileOfferAck ack {};
            if (acknowledged || !plank_clipboard_file_offer_ack_decode(
                    record->payload, record->payload_size, &ack)) return SourceResult::Failed;
            acknowledged = true;
            wire.allowHostPreemption(false);
        } else if (record->type == PLANK_CLIPBOARD_FILE_MATERIALIZE) {
            if (!acknowledged || record->flags != PLANK_CLIPBOARD_FILE_FLAG_ALL_ITEMS ||
                    !plank_clipboard_file_materialization_decode(
                        record->payload, record->payload_size, &materialization)) return SourceResult::Failed;
        } else if (record->type == PLANK_CLIPBOARD_FILE_CANCEL ||
                   record->type == PLANK_CLIPBOARD_FILE_ERROR) {
            peerTerminated = true;
            return SourceResult::Failed;
        } else {
            return SourceResult::Failed;
        }
    }

    for (const auto& entry : entries) {
        if (cancelled()) return SourceResult::Failed;
        if (entry.kind != PLANK_CLIPBOARD_FILE_REGULAR) continue;
        QCryptographicHash digest(QCryptographicHash::Sha256);
        std::uint64_t offset = 0;
        HANDLE fd = entry.handle.get();
        if (!fd || !statStable(fd, entry)) return SourceResult::Failed;
        while (offset < entry.size) {
            if (cancelled()) return SourceResult::Failed;
            const auto record = wire.receiveControl(storage, 100);
            if (!record) {
                if (wire.failed() || m_Stop.load(std::memory_order_acquire)) {
                    return SourceResult::Failed;
                }
                continue;
            }
            if (record->type != PLANK_CLIPBOARD_FILE_READ_REQUEST) {
                peerTerminated = record->type == PLANK_CLIPBOARD_FILE_CANCEL || record->type == PLANK_CLIPBOARD_FILE_ERROR;
                return SourceResult::Failed;
            }
            PlankClipboardFileRange range {};
            if (!plank_clipboard_file_decode_range(
                    record->payload, record->payload_size, 0, &range) ||
                    range.materialization_id != materialization ||
                    range.entry_id != entry.id || range.offset != offset ||
                    range.size > entry.size - offset) {
                return SourceResult::Failed;
            }
            std::vector<std::uint8_t> payload(
                    PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES + range.size);
            if (!plank_clipboard_file_range_header(payload.data(), materialization,
                    entry.id, offset, range.size)) {
                return SourceResult::Failed;
            }
            DWORD count = 0;
            if (!ReadFile(fd, payload.data() + PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES,
                          range.size, &count, nullptr) || count != range.size) return SourceResult::Failed;
            digest.addData(QByteArrayView(reinterpret_cast<const char*>(
                    payload.data() + PLANK_CLIPBOARD_FILE_DATA_PREFIX_BYTES), range.size));
            if (!wire.sendData(payload.data(), payload.size())) {
                return SourceResult::Failed;
            }
            offset += range.size;
        }
        if (!statStable(fd, entry)) return SourceResult::Failed;
        const QByteArray hash = digest.result();
        PlankClipboardFileEntryComplete complete {
            materialization, entry.id, entry.size, {0}
        };
        std::memcpy(complete.sha256, hash.constData(), sizeof(complete.sha256));
        std::array<std::uint8_t, PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE_BYTES> payload {};
        if (!plank_clipboard_file_entry_complete_encode(payload.data(), &complete) ||
                !wire.sendControl(PLANK_CLIPBOARD_FILE_ENTRY_COMPLETE, 0,
                                  payload.data(), payload.size())) return SourceResult::Failed;
    }

    while (!cancelled()) {
        const auto record = wire.receiveControl(storage, 100);
        if (!record) {
            if (wire.failed()) return SourceResult::Failed;
            continue;
        }
        std::uint64_t ready = 0;
        if (record->type == PLANK_CLIPBOARD_FILE_TRANSFER_READY &&
                plank_clipboard_file_materialization_decode(
                    record->payload, record->payload_size, &ready) &&
                ready == materialization) {
            for (const auto& entry : entries)
                if (!statStable(entry.handle.get(), entry)) return SourceResult::Failed;
            complete = true;
            return SourceResult::Ready;
        }
        if (record->type == PLANK_CLIPBOARD_FILE_CANCEL ||
                record->type == PLANK_CLIPBOARD_FILE_ERROR) {
            peerTerminated = true;
            return SourceResult::Failed;
        }
        return SourceResult::Failed;
    }
    return SourceResult::Failed;
}

bool WindowsFileClipboard::receiveHostOffer(std::vector<std::uint8_t> firstRecord)
{
    const auto clipboardSequenceAtOffer = GetClipboardSequenceNumber();
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
    std::set<QString> foldedPaths;
    std::uint64_t totalBytes = 0;
    std::uint32_t topLevelCount = 0;
    std::size_t manifestBytes = first.payload_size;
    std::vector<std::uint8_t> storage;
    bool valid = true;
    for (std::uint32_t index = 0; valid && index < begin.entry_count; ++index) {
        std::optional<PlankClipboardFileRecord> record;
        while (valid && !record && !m_Stop.load(std::memory_order_acquire)) {
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
        if (!plank::clipboard::windows::validName(entry.name)) { valid = false; break; }
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
                !foldedPaths.insert(plank::clipboard::windows::foldedPath(relative)).second ||
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

    if (m_StagingRoot.empty()) {
        const auto baseString = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const fs::path base(baseString.toStdWString());
        const auto sessionRoot = base / (L"Fernweh-files-" +
                QString::fromStdString(offerName(offer)).toStdWString());
        if (baseString.isEmpty() || !plank::clipboard::windows::plainAbsolutePath(base) ||
                !createPrivateDirectory(sessionRoot)) {
            sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
            return false;
        }
        m_StagingRoot = sessionRoot.u8string();
    }
    const fs::path root = fs::u8path(m_StagingRoot);
    const fs::path partial = root / (offerName(offer) + ".partial");
    const fs::path tree = partial / "tree";
    const fs::path blobs = partial / "blobs";
    const fs::path completed = root / offerName(offer);
    std::error_code error;
    if (!plank::clipboard::windows::plainAbsolutePath(root) ||
            !createPrivateDirectory(partial)) {
        sendError(PLANK_CLIPBOARD_FILE_REASON_DESTINATION_UNAVAILABLE);
        return false;
    }
    if (!createPrivateDirectory(tree) || !createPrivateDirectory(blobs) ||
            !hasSpace(root, totalBytes)) {
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
        HANDLE fd = CreateFileW(blob.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (fd == INVALID_HANDLE_VALUE) {
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
                FlushFileBuffers(fd) &&
                setFileMtime(fd, entry.mtimeNs);
        CloseHandle(fd);
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
        if (!setPathMtime(path, iterator->mtimeNs)) {
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
        m_HostClipboardSequence = clipboardSequenceAtOffer;
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
        m_Changed.wait_for(lock, 60s, [&] {
            return m_Stop.load(std::memory_order_acquire) || m_HostPublishComplete;
        });
        m_HostPublishPending = false;
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
