#include "macclipboardsync.h"

#include <AppKit/AppKit.h>
#include <SDL3/SDL.h>

namespace {

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
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
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

void writeGeneralPasteboardText(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:bytes.data()
                                                  length:bytes.size()
                                                encoding:NSUTF8StringEncoding];
        if (text == nil) {
            return;
        }
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        [pasteboard setString:text forType:NSPasteboardTypeString];
    }
}

int currentPasteboardChangeCount()
{
    @autoreleasepool {
        NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
        return pasteboard != nil ? pasteboard.changeCount : -1;
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
    m_LastSentText.clear();
    m_LastAppliedHostText.clear();
}

void MacClipboardSync::stop()
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_Running) {
        return;
    }
    m_Running = false;
    m_ApplyingRemote = false;
    m_Assembly.reset();
    m_PendingHostText.reset();
    m_LastPasteboardChangeCount = -1;
    m_OutboundGeneration = 0;
    m_LastAppliedHostGeneration = 0;
    m_LastSentText.clear();
    m_LastAppliedHostText.clear();
}

void MacClipboardSync::pollLocalClipboardOnMainThread()
{
    if (!m_IsEnabled() || !m_HasStreamFocus()) {
        return;
    }
    const auto changeCount = currentPasteboardChangeCount();
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || m_ApplyingRemote ||
                (changeCount >= 0 && changeCount == m_LastPasteboardChangeCount)) {
            return;
        }
        m_LastPasteboardChangeCount = changeCount;
    }
    const std::string text = readGeneralPasteboardText();
    if (text.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || text == m_LastAppliedHostText) {
            return;
        }
    }
    sendLocalClipboard(text);
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
        pending.text = std::move(m_Assembly.bytes);
        m_Assembly.reset();

        const std::string completedText(
                    reinterpret_cast<const char*>(pending.text.data()),
                    pending.text.size());
        m_LastAppliedHostGeneration = generation;
        if (completedText == m_LastAppliedHostText ||
                (m_PendingHostText.has_value() &&
                 m_PendingHostText->sessionEpoch == m_SessionEpoch &&
                 completedText == std::string(
                     reinterpret_cast<const char*>(m_PendingHostText->text.data()),
                     m_PendingHostText->text.size()))) {
            return true;
        }
        m_PendingHostText = std::move(pending);
    }

    if (m_QueueHostText) {
        m_QueueHostText();
    }
    return true;
}

void MacClipboardSync::sendLocalClipboard(const std::string& text)
{
    if (!m_IsEnabled() || !m_HasStreamFocus() || text.empty()) {
        return;
    }
    std::uint64_t generation = 0;
    std::uint64_t sessionEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || text == m_LastSentText) {
            return;
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
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Running || m_SessionEpoch != sessionEpoch) {
            return;
        }
        m_LastSentText = text;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Sent clipboard offer to host (%zu bytes, generation %llu)",
                text.size(),
                static_cast<unsigned long long>(generation));
}

bool MacClipboardSync::applyPendingHostTextOnMainThread()
{
    std::lock_guard<std::mutex> lock(m_StateMutex);
    if (!m_Running || !m_PendingHostText.has_value() ||
            m_PendingHostText->sessionEpoch != m_SessionEpoch) {
        m_PendingHostText.reset();
        return false;
    }

    const auto pending = std::move(*m_PendingHostText);
    m_PendingHostText.reset();
    const std::string incoming(
                reinterpret_cast<const char*>(pending.text.data()),
                pending.text.size());
    if (incoming == m_LastAppliedHostText) {
        return false;
    }

    m_ApplyingRemote = true;
    writeGeneralPasteboardText(pending.text);
    m_LastAppliedHostText = incoming;
    m_LastPasteboardChangeCount = currentPasteboardChangeCount();
    m_ApplyingRemote = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Applied host clipboard offer (%zu bytes)",
                pending.text.size());
    return true;
}
