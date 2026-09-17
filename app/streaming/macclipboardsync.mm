#include "macclipboardsync.h"

#include <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#ifdef PLANK_CLIPBOARD_TEST_PASTEBOARD
// Supplied only by the native test binary; never linked into the application.
extern NSPasteboard* plankClipboardTestPasteboard();
#endif

namespace {

NSPasteboard* clipboardPasteboard()
{
#ifdef PLANK_CLIPBOARD_TEST_PASTEBOARD
    return plankClipboardTestPasteboard();
#else
    return [NSPasteboard generalPasteboard];
#endif
}

NSArray<NSString*>* pasteboardTextTypes()
{
    return @[
        NSPasteboardTypeString,
        @"public.utf8-plain-text",
        @"public.plain-text",
    ];
}

std::string readGeneralPasteboardText()
{
    @autoreleasepool {
        NSPasteboard* pasteboard = clipboardPasteboard();
        if (pasteboard == nil) {
            return {};
        }
        for (NSString* type in pasteboardTextTypes()) {
            NSString* text = [pasteboard stringForType:type];
            if (text != nil && text.length > 0) {
                const char* utf8 = [text UTF8String];
                if (utf8 != nullptr && utf8[0] != '\0') {
                    return std::string {utf8};
                }
            }
        }
    }
    return {};
}

bool writeGeneralPasteboardText(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return false;
    }
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:bytes.data()
                                                  length:bytes.size()
                                                encoding:NSUTF8StringEncoding];
        if (text == nil) {
            return false;
        }
        NSPasteboard* pasteboard = clipboardPasteboard();
        [pasteboard clearContents];
        return [pasteboard setString:text forType:NSPasteboardTypeString];
    }
}

int currentPasteboardChangeCount()
{
    @autoreleasepool {
        NSPasteboard* pasteboard = clipboardPasteboard();
        return pasteboard != nil ? pasteboard.changeCount : -1;
    }
}

// stop() also runs on connection/cleanup workers. Keep value-only cleanup
// records alive independently of the sync object; never block a worker waiting
// for the main thread that may be joining it. A new session drains these before
// reading the pasteboard, even if the queued main-thread block has not run yet.
struct RemotePasteboardCleanup {
    std::int64_t changeCount;
    std::string text;
};
std::mutex cleanupMutex;
std::vector<RemotePasteboardCleanup> pendingCleanup;

void clearStoppedRemotePasteboardsOnMainThread()
{
    SDL_assert([NSThread isMainThread]);
    std::vector<RemotePasteboardCleanup> work;
    {
        std::lock_guard<std::mutex> lock(cleanupMutex);
        work.swap(pendingCleanup);
    }
    @autoreleasepool {
        for (const auto& entry : work) {
            NSPasteboard* pasteboard = clipboardPasteboard();
            if (pasteboard != nil && pasteboard.changeCount == entry.changeCount &&
                    readGeneralPasteboardText() == entry.text &&
                    pasteboard.changeCount == entry.changeCount) {
                [pasteboard clearContents];
            }
        }
    }
}

}  // namespace

#ifdef Q_OS_MACOS
char* macReadGeneralPasteboardTextForSdl()
{
    const std::string text = readGeneralPasteboardText();
    if (text.empty()) {
        return nullptr;
    }
    return SDL_strdup(text.c_str());
}
#endif

MacClipboardSync::MacClipboardSync(SendInputFrame sendInputFrame,
                                   FocusPredicate hasStreamFocus,
                                   EnabledPredicate isEnabled,
                                   QueueHostText queueHostText)
    : m_SendInputFrame(std::move(sendInputFrame)),
      m_HasStreamFocus(std::move(hasStreamFocus)),
      m_IsEnabled(std::move(isEnabled)),
      m_QueueHostText(std::move(queueHostText))
{
}

MacClipboardSync::~MacClipboardSync()
{
    stop();
}

void MacClipboardSync::start()
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (m_Running) {
        return;
    }
    m_Running = true;
    ++m_SessionEpoch;
    m_ApplyingRemote = false;
    m_Assembly.reset();
    m_PendingHostText.reset();
    m_LastPasteboardChangeCount = -1;
    m_OutboundGeneration = 0;
    m_LastAppliedHostGeneration = 0;
    m_LastAppliedHostText.clear();
    m_RemotePasteboardChangeCount = -1;
}

void MacClipboardSync::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running) {
            return;
        }
        m_Running = false;
        if (m_RemotePasteboardChangeCount >= 0) {
            std::lock_guard<std::mutex> cleanupLock(cleanupMutex);
            pendingCleanup.push_back({m_RemotePasteboardChangeCount,
                                      std::move(m_LastAppliedHostText)});
        }
        m_ApplyingRemote = false;
        m_Assembly.reset();
        m_PendingHostText.reset();
        m_LastPasteboardChangeCount = -1;
        m_RemotePasteboardChangeCount = -1;
        m_OutboundGeneration = 0;
        m_LastAppliedHostGeneration = 0;
        m_LastAppliedHostText.clear();
    }
    if ([NSThread isMainThread]) {
        clearStoppedRemotePasteboardsOnMainThread();
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{
            clearStoppedRemotePasteboardsOnMainThread();
        });
    }
}

void MacClipboardSync::pollLocalClipboardOnMainThread()
{
    clearStoppedRemotePasteboardsOnMainThread();
    if (!m_IsEnabled() || !m_HasStreamFocus()) {
        return;
    }
    const auto changeCount = currentPasteboardChangeCount();
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || m_ApplyingRemote || changeCount < 0 ||
                changeCount == m_LastPasteboardChangeCount) {
            return;
        }
        epoch = m_SessionEpoch;
        if (changeCount != m_RemotePasteboardChangeCount) {
            // A new local copy owns the pasteboard, even if its bytes equal an
            // earlier remote offer. It must not be suppressed or cleared later.
            m_RemotePasteboardChangeCount = -1;
            m_LastAppliedHostText.clear();
        }
    }
    const std::string text = readGeneralPasteboardText();
    if (currentPasteboardChangeCount() != changeCount) {
        return; // Retry a stable snapshot on the next poll.
    }
    if (text.empty() || sendLocalClipboard(text, epoch)) {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (m_Running && m_SessionEpoch == epoch) {
            m_LastPasteboardChangeCount = changeCount;
        }
    }
}

bool MacClipboardSync::handleHostOffer(const std::uint8_t* data, std::size_t length)
{
    if (data == nullptr || length < sizeof(PLANK_CLIPBOARD_WIRE_HEADER) ||
            length > sizeof(PLANK_CLIPBOARD_WIRE_HEADER) +
                PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE) {
        return false;
    }

    PLANK_CLIPBOARD_WIRE_HEADER wire {};
    std::memcpy(&wire, data, sizeof(wire));
    const auto chunkSize = qFromLittleEndian(wire.chunkSize);
    if (chunkSize > PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE ||
            length != sizeof(wire) + chunkSize) {
        return false;
    }

    const auto generation = qFromLittleEndian(wire.generation);
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running) {
            return true;
        }
        if (generation <= m_LastAppliedHostGeneration) {
            return true;
        }
        const auto result = m_Assembly.appendChunk(wire, data + sizeof(wire));
        if (result == plank::clipboard::AppendResult::Rejected) {
            return false;
        }
        if (result == plank::clipboard::AppendResult::Incomplete) {
            return true;
        }
        PendingHostText pending;
        pending.sessionEpoch = m_SessionEpoch;
        pending.generation = generation;
        epoch = m_SessionEpoch;
        pending.text = std::move(m_Assembly.bytes);
        m_Assembly.reset();

        m_LastAppliedHostGeneration = generation;
        // Every complete newer offer supersedes pending work, including a
        // return to the last applied string. AppKit deduplication happens on
        // the main thread using the current pasteboard change count.
        const bool alreadyQueued = m_PendingHostText.has_value();
        m_PendingHostText = std::move(pending);
        if (alreadyQueued) {
            return true;
        }
    }

    if (!m_QueueHostText || !m_QueueHostText()) {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (m_PendingHostText.has_value() &&
                m_PendingHostText->sessionEpoch == epoch &&
                m_PendingHostText->generation == generation) {
            m_PendingHostText.reset();
        }
        return false;
    }
    return true;
}

bool MacClipboardSync::sendLocalClipboard(const std::string& text, std::uint64_t expectedEpoch)
{
    if (!m_IsEnabled() || !m_HasStreamFocus() || text.empty()) {
        return false;
    }
    std::uint64_t generation = 0;
    std::uint64_t sessionEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || m_SessionEpoch != expectedEpoch) {
            return false;
        }
        generation = ++m_OutboundGeneration;
        sessionEpoch = m_SessionEpoch;
    }

    const auto frames = plank::clipboard::buildEventFrames(
            reinterpret_cast<const std::uint8_t*>(text.data()),
            text.size(),
            generation,
            PLANK_CLIPBOARD_MAX_INPUT_CHUNK_SIZE);
    for (const auto& frame : frames) {
        if (!m_SendInputFrame(frame.data(), frame.size())) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to send clipboard offer to host");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || m_SessionEpoch != sessionEpoch) {
            return false;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Sent clipboard offer to host (%zu bytes, generation %llu)",
                text.size(),
                static_cast<unsigned long long>(generation));
    return true;
}

bool MacClipboardSync::applyPendingHostTextOnMainThread()
{
    clearStoppedRemotePasteboardsOnMainThread();
    std::lock_guard<std::mutex> lock(m_StateMutex);
    // A queued Host update must not take the clipboard from another Mac app.
    // Discard it instead of replaying stale text when stream focus returns.
    if (!m_Running || !m_IsEnabled() || !m_HasStreamFocus() ||
            !m_PendingHostText.has_value() ||
            m_PendingHostText->sessionEpoch != m_SessionEpoch) {
        m_PendingHostText.reset();
        return false;
    }

    const auto pending = std::move(*m_PendingHostText);
    m_PendingHostText.reset();
    const std::string incoming(
                reinterpret_cast<const char*>(pending.text.data()),
                pending.text.size());
    if (incoming == m_LastAppliedHostText &&
            currentPasteboardChangeCount() == m_RemotePasteboardChangeCount) {
        return false;
    }

    m_ApplyingRemote = true;
    if (!writeGeneralPasteboardText(pending.text)) {
        m_ApplyingRemote = false;
        return false;
    }
    m_LastAppliedHostText = incoming;
    m_LastPasteboardChangeCount = currentPasteboardChangeCount();
    m_RemotePasteboardChangeCount = m_LastPasteboardChangeCount;
    m_ApplyingRemote = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Applied host clipboard offer (%zu bytes)",
                pending.text.size());
    return true;
}
