#include "macclipboardsync.h"

#include <AppKit/AppKit.h>
#include <SDL3/SDL.h>

namespace {

std::string readGeneralPasteboardText()
{
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard == nil) {
        return {};
    }
    NSString* text = [pasteboard stringForType:NSPasteboardTypeString];
    if (text == nil || text.length == 0) {
        return {};
    }
    return std::string {[text UTF8String]};
}

void writeGeneralPasteboardText(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }
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

}  // namespace

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
    if (m_Running.exchange(true)) {
        return;
    }
    m_LastPasteboardChangeCount = -1;
    m_PollThread = std::thread([this] { pollLoop(); });
}

void MacClipboardSync::stop()
{
    if (!m_Running.exchange(false)) {
        return;
    }
    if (m_PollThread.joinable()) {
        m_PollThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        m_Assembly.reset();
    }
    m_LastPasteboardChangeCount = -1;
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
    const auto generation = qFromLittleEndian(wire.generation);
    if (generation <= m_LastAppliedHostGeneration ||
            generation == m_LastSentGeneration) {
        return true;
    }

    std::vector<std::uint8_t> completed;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (!m_Assembly.appendChunk(wire, data + sizeof(wire))) {
            return true;
        }
        completed = std::move(m_Assembly.bytes);
        m_Assembly.reset();
    }

    m_LastAppliedHostGeneration = generation;
    if (m_QueueHostText) {
        m_QueueHostText(std::move(completed));
    }
    return true;
}

void MacClipboardSync::pollLoop()
{
    while (m_Running.load()) {
        if (m_IsEnabled() && m_HasStreamFocus() && !m_ApplyingRemote.load()) {
            NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
            const auto changeCount = pasteboard != nil ? pasteboard.changeCount : -1;
            if (changeCount >= 0 && changeCount != m_LastPasteboardChangeCount) {
                m_LastPasteboardChangeCount = changeCount;
                const std::string text = readGeneralPasteboardText();
                if (!text.empty()) {
                    sendLocalClipboard(text);
                }
            }
        }
        SDL_Delay(250);
    }
}

void MacClipboardSync::sendLocalClipboard(const std::string& text)
{
    if (!m_IsEnabled() || !m_HasStreamFocus() || text.empty()) {
        return;
    }
    const auto generation = ++m_OutboundGeneration;
    m_LastSentGeneration = generation;
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Sent clipboard offer to host (%zu bytes, generation %llu)",
                text.size(),
                static_cast<unsigned long long>(generation));
}

void MacClipboardSync::applyHostTextOnMainThread(const std::vector<std::uint8_t>& text)
{
    m_ApplyingRemote.store(true);
    writeGeneralPasteboardText(text);
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    if (pasteboard != nil) {
        m_LastPasteboardChangeCount = pasteboard.changeCount;
    }
    m_ApplyingRemote.store(false);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Applied host clipboard offer (%zu bytes)",
                text.size());
}
