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
        for (NSString* type in [pasteboard types]) {
            if (![type hasPrefix:@"public."] &&
                    ![type isEqualToString:@"NSStringPboardType"]) {
                continue;
            }
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
                                   EnabledPredicate isEnabled,
                                   QueueHostText queueHostText)
    : m_SendInputFrame(std::move(sendInputFrame)),
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
    m_LastSentText.clear();
    m_LastAppliedHostText.clear();
}

void MacClipboardSync::stop()
{
    if (!m_Running.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        m_Assembly.reset();
    }
    m_LastPasteboardChangeCount = -1;
    m_LastSentText.clear();
    m_LastAppliedHostText.clear();
}

void MacClipboardSync::pollLocalClipboardOnMainThread()
{
    if (!m_Running.load() || !m_IsEnabled() || m_ApplyingRemote.load()) {
        return;
    }
    const auto changeCount = currentPasteboardChangeCount();
    if (changeCount >= 0 && changeCount == m_LastPasteboardChangeCount) {
        return;
    }
    m_LastPasteboardChangeCount = changeCount;
    const std::string text = readGeneralPasteboardText();
    if (text.empty()) {
        return;
    }
    std::string lastAppliedHostText;
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        lastAppliedHostText = m_LastAppliedHostText;
    }
    if (text == lastAppliedHostText) {
        return;
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

    const std::string completedText(
                reinterpret_cast<const char*>(completed.data()),
                completed.size());
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (completedText == m_LastAppliedHostText) {
            m_LastAppliedHostGeneration = generation;
            return true;
        }
    }

    m_LastAppliedHostGeneration = generation;
    if (m_QueueHostText) {
        m_QueueHostText(std::move(completed));
    }
    return true;
}

void MacClipboardSync::sendLocalClipboard(const std::string& text)
{
    if (!m_IsEnabled() || text.empty() || text == m_LastSentText) {
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
    m_LastSentText = text;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Sent clipboard offer to host (%zu bytes, generation %llu)",
                text.size(),
                static_cast<unsigned long long>(generation));
}

void MacClipboardSync::applyHostTextOnMainThread(const std::vector<std::uint8_t>& text)
{
    const std::string incoming(
                reinterpret_cast<const char*>(text.data()),
                text.size());
    {
        std::lock_guard<std::mutex> lock(m_StateMutex);
        if (incoming == m_LastAppliedHostText) {
            return;
        }
        m_LastAppliedHostText = incoming;
    }

    m_ApplyingRemote.store(true);
    writeGeneralPasteboardText(text);
    m_LastPasteboardChangeCount = currentPasteboardChangeCount();
    if (!incoming.empty()) {
        SDL_SetClipboardText(incoming.c_str());
    }
    m_ApplyingRemote.store(false);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Applied host clipboard offer (%zu bytes)",
                text.size());
}
