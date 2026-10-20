#include "clipboardsync.h"
#include "clipboardplatform.h"
#include <SDL3/SDL.h>

namespace {
using ClipboardPlatform::changeCount;
std::int64_t currentPasteboardChangeCount() { return changeCount(); }
struct RemotePasteboardCleanup {
    std::int64_t changeCount;
    std::string text;
};
std::mutex cleanupMutex;
std::vector<RemotePasteboardCleanup> pendingCleanup;
void clearStoppedRemotePasteboardsOnMainThread()
{
    std::vector<RemotePasteboardCleanup> work;
    {
        std::lock_guard<std::mutex> lock(cleanupMutex);
        work.swap(pendingCleanup);
    }
    for (const auto& entry : work) {
        ClipboardPlatform::clearIfMatches(entry.changeCount, entry.text);
    }
}
} // namespace

ClipboardSync::ClipboardSync(SendInputFrame sendInputFrame,
                                   FocusPredicate hasStreamFocus,
                                   EnabledPredicate isEnabled,
                                   QueueHostText queueHostText)
    : m_SendInputFrame(std::move(sendInputFrame)),
      m_HasStreamFocus(std::move(hasStreamFocus)),
      m_IsEnabled(std::move(isEnabled)),
      m_QueueHostText(std::move(queueHostText))
{
}

ClipboardSync::~ClipboardSync()
{
    stop();
}

void ClipboardSync::start()
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
    m_OutgoingText.clear(); m_OutgoingFrames.clear(); m_NextOutgoingFrame = 0;
    m_LastAppliedHostGeneration = 0;
    m_LastAppliedHostText.clear();
    m_RemotePasteboardChangeCount = -1;
}

void ClipboardSync::stop()
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
        m_OutgoingText.clear(); m_OutgoingFrames.clear(); m_NextOutgoingFrame = 0;
        m_LastAppliedHostGeneration = 0;
        m_LastAppliedHostText.clear();
    }
    ClipboardPlatform::runOnMainThread(clearStoppedRemotePasteboardsOnMainThread);
}

bool ClipboardSync::pollLocalClipboardOnMainThread()
{
    applyPendingHostTextOnMainThread();
    if (!m_IsEnabled() || !m_HasStreamFocus()) {
        return true;
    }
    const auto changeCount = currentPasteboardChangeCount();
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (m_PendingHostText) return false;
        if (!m_Running || m_ApplyingRemote || changeCount < 0 ||
                changeCount == m_LastPasteboardChangeCount) {
            return true;
        }
        epoch = m_SessionEpoch;
        if (changeCount != m_RemotePasteboardChangeCount) {
            // A new local copy owns the pasteboard, even if its bytes equal an
            // earlier remote offer. It must not be suppressed or cleared later.
            m_RemotePasteboardChangeCount = -1;
            m_LastAppliedHostText.clear();
        }
    }
    const auto snapshot = ClipboardPlatform::readText();
    if (!snapshot || currentPasteboardChangeCount() != changeCount) {
        return false; // Retry a stable snapshot on the next poll.
    }
    const auto& text = *snapshot;
    if (text.empty() || sendLocalClipboard(text, epoch)) {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (m_Running && m_SessionEpoch == epoch) {
            if (text.empty()) { m_OutgoingText.clear(); m_OutgoingFrames.clear(); m_NextOutgoingFrame = 0; }
            m_LastPasteboardChangeCount = changeCount;
        }
        return true;
    }
    return false;
}

bool ClipboardSync::handleHostOffer(const std::uint8_t* data, std::size_t length)
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

    PlankClipboardChunk chunk {};
    if (!plank_clipboard_decode(data, length, PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE, &chunk)) return false;
    const auto generation = chunk.generation;
    std::uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running) {
            return true;
        }
        if (generation <= m_LastAppliedHostGeneration) {
            return true;
        }
        const auto result = m_Assembly.appendChunk(chunk);
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
        // return to the last applied string. Clipboard deduplication happens on
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

bool ClipboardSync::sendLocalClipboard(const std::string& text, std::uint64_t expectedEpoch)
{
    if (!m_IsEnabled() || !m_HasStreamFocus() ||
            !plank::clipboard::validUtf8(text.data(), text.size())) return false;
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_Running || m_SessionEpoch != expectedEpoch) return false;
    if (m_OutgoingFrames.empty() || m_OutgoingText != text) {
        m_OutgoingFrames = plank::clipboard::buildEventFrames(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
            ++m_OutboundGeneration, PLANK_CLIPBOARD_MAX_INPUT_CHUNK_SIZE);
        if (m_OutgoingFrames.empty()) return false;
        m_OutgoingText = text;
        m_NextOutgoingFrame = 0;
    }
    // One bounded copy (at most 65 input chunks), using only nonblocking queue
    // operations. Do not deliberately interleave a following paste keystroke
    // with this transfer. Queue pressure resumes at precisely the unsent chunk.
    while (m_NextOutgoingFrame < m_OutgoingFrames.size()) {
        const auto& frame = m_OutgoingFrames[m_NextOutgoingFrame];
        if (!m_SendInputFrame(frame.data(), frame.size())) return false;
        ++m_NextOutgoingFrame;
    }
    if (m_NextOutgoingFrame != m_OutgoingFrames.size()) return false;
    m_OutgoingFrames.clear(); m_OutgoingText.clear(); m_NextOutgoingFrame = 0;
    return true;
}

bool ClipboardSync::applyPendingHostTextOnMainThread()
{
    clearStoppedRemotePasteboardsOnMainThread();
    std::lock_guard<std::mutex> lock(m_StateMutex);
    // A queued Host update must not take the clipboard from another local app.
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
    if (!ClipboardPlatform::writeText(pending.text)) {
        m_ApplyingRemote = false;
        m_PendingHostText = pending; // Clipboard may be busy; poll retries.
        return false;
    }
    m_LastAppliedHostText = incoming;
    m_OutgoingFrames.clear(); m_OutgoingText.clear(); m_NextOutgoingFrame = 0;
    m_LastPasteboardChangeCount = currentPasteboardChangeCount();
    m_RemotePasteboardChangeCount = m_LastPasteboardChangeCount;
    m_ApplyingRemote = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Applied host clipboard offer (%zu bytes)",
                pending.text.size());
    return true;
}
