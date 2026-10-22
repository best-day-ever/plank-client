#include "clipboardplatform.h"
#include "plankclipboard.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>

namespace {
// SetClipboardData requires an actual owner. Construct only on the SDL UI
// thread; this message-only window never steals focus or becomes visible.
HWND ownerWindow()
{
    struct Owner {
        HWND window = CreateWindowExW(0, L"STATIC", L"Fernweh clipboard", 0,
                                      0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        ~Owner() { if (window) DestroyWindow(window); }
    };
    static thread_local Owner owner;
    return owner.window;
}
struct ClipboardLock {
    bool locked;
    explicit ClipboardLock(bool writing = false)
        : locked(writing ? (ownerWindow() && OpenClipboard(ownerWindow())) : OpenClipboard(nullptr)) {}
    ~ClipboardLock() { if (locked) CloseClipboard(); }
};
std::optional<std::string> readLockedText()
{
    // Explorer exposes filenames as text as well. Only the file protocol may
    // share these, and only when the Host grants the separate entitlement.
    if (IsClipboardFormatAvailable(CF_HDROP) || !IsClipboardFormatAvailable(CF_UNICODETEXT))
        return std::string{};
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) return std::nullopt;
    const SIZE_T capacity = GlobalSize(data) / sizeof(wchar_t);
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) return std::nullopt;
    SIZE_T length = 0;
    // A UTF-8 scalar never needs fewer bytes than UTF-16 code units. Bound
    // scans before allocating, including malformed/nonterminated buffers.
    while (length < capacity && length <= PLANK_CLIPBOARD_MAX_TEXT_SIZE && text[length]) ++length;
    std::optional<std::string> result = std::string{};
    if (length < capacity && length <= PLANK_CLIPBOARD_MAX_TEXT_SIZE && length != 0) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                            static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (size > 0 && size <= PLANK_CLIPBOARD_MAX_TEXT_SIZE) {
            std::string value(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                    static_cast<int>(length), value.data(), size, nullptr, nullptr) == size &&
                    plank_clipboard_valid_text(reinterpret_cast<const uint8_t*>(value.data()), value.size()))
                result = std::move(value);
        }
    }
    GlobalUnlock(data);
    return result;
}
} // namespace

namespace ClipboardPlatform {
std::optional<std::string> readText()
{
    ClipboardLock lock;
    return lock.locked ? readLockedText() : std::nullopt;
}
std::int64_t changeCount()
{
    // Zero means unavailable (for example a noninteractive desktop).
    const auto sequence = GetClipboardSequenceNumber();
    return sequence ? static_cast<std::int64_t>(sequence) : -1;
}
bool writeText(const std::vector<std::uint8_t>& bytes)
{
    if (!plank_clipboard_valid_text(bytes.data(), bytes.size())) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return false;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (static_cast<SIZE_T>(length) + 1) * sizeof(wchar_t));
    if (!data) return false;
    auto* text = static_cast<wchar_t*>(GlobalLock(data));
    if (!text) { GlobalFree(data); return false; }
    const bool converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), text, length) == length;
    text[length] = L'\0';
    GlobalUnlock(data);
    ClipboardLock lock(true);
    if (!converted || !lock.locked || !EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, data)) {
        GlobalFree(data);
        return false;
    }
    return true; // Windows owns the allocation now.
}
void clearIfMatches(std::int64_t sequence, const std::string& text)
{
    ClipboardLock lock(true);
    if (!lock.locked || changeCount() != sequence) return;
    const auto current = readLockedText();
    if (current && *current == text && changeCount() == sequence) EmptyClipboard();
}
void runOnMainThread(std::function<void()> callback)
{
    auto* app = QCoreApplication::instance();
    if (app && QThread::currentThread() == app->thread()) callback();
    else if (app) QMetaObject::invokeMethod(app, std::move(callback), Qt::QueuedConnection);
    // Value-only cleanup also drains on the next main-thread poll/start, so
    // shutdown never waits on a UI thread that is joining a receiver worker.
}
} // namespace ClipboardPlatform
