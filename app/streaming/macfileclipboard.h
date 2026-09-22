#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct PlankTransportNativeEndpoint;

/**
 * Session-scoped macOS file clipboard source for Client-to-Host transfers.
 * AppKit pasteboard access remains on the main thread; enumeration and file IO
 * run on the private worker.
 */
class MacFileClipboard {
public:
    using ReadyCallback = std::function<void()>;

    MacFileClipboard(PlankTransportNativeEndpoint* endpoint,
                     std::string mode,
                     ReadyCallback readyCallback);
    ~MacFileClipboard();

    void start();
    void stop();
    void pollLocalClipboardOnMainThread();

    /**
     * Consume a remote paste gesture when stable local file URLs are present.
     * The withheld paste is released through ReadyCallback only after the Host
     * has staged and published the files.
     */
    bool beginPasteOnMainThread();

private:
    struct PasteboardSnapshot {
        std::int64_t changeCount = -1;
        std::vector<std::string> paths;
    };

    void workerLoop();
    bool transferSnapshot(const PasteboardSnapshot& snapshot);
    bool clientToHostAllowed() const;

    PlankTransportNativeEndpoint* m_Endpoint;
    std::string m_Mode;
    ReadyCallback m_ReadyCallback;
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::thread m_Worker;
    std::atomic_bool m_Stop {false};
    bool m_Running = false;
    bool m_PasteRequested = false;
    PasteboardSnapshot m_Current;
    PasteboardSnapshot m_Requested;
};
