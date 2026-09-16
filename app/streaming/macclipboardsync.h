#pragma once

#include "plankclipboard.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#ifdef Q_OS_MACOS
// Returns SDL_malloc'd UTF-8 text, or nullptr when unavailable.
char* macReadGeneralPasteboardTextForSdl();
#endif

class MacClipboardSync {
public:
    using SendInputFrame = std::function<bool(const std::uint8_t*, std::size_t)>;
    using EnabledPredicate = std::function<bool()>;
    using QueueHostText = std::function<void(std::vector<std::uint8_t>)>;

    MacClipboardSync(SendInputFrame sendInputFrame,
                     EnabledPredicate isEnabled,
                     QueueHostText queueHostText);
    ~MacClipboardSync();

    void start();
    void stop();
    bool handleHostOffer(const std::uint8_t* data, std::size_t length);
    void applyHostTextOnMainThread(const std::vector<std::uint8_t>& text);
    void pollLocalClipboardOnMainThread();

private:
    void sendLocalClipboard(const std::string& text);

    SendInputFrame m_SendInputFrame;
    EnabledPredicate m_IsEnabled;
    QueueHostText m_QueueHostText;
    std::atomic_bool m_Running {false};
    std::atomic_bool m_ApplyingRemote {false};
    std::mutex m_StateMutex;
    plank::clipboard::Assembly m_Assembly;
    std::int64_t m_LastPasteboardChangeCount = -1;
    std::uint64_t m_OutboundGeneration = 0;
    std::uint64_t m_LastAppliedHostGeneration = 0;
    std::uint64_t m_LastSentGeneration = 0;
    std::string m_LastSentText;
    std::string m_LastAppliedHostText;
};
