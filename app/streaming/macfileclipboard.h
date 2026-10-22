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
 * Session-scoped bidirectional macOS file clipboard and explicit file picker.
 * AppKit pasteboard access remains on the main thread; enumeration and file IO
 * run on the private worker.
 */
class MacFileClipboard {
public:
    using ReadyCallback = std::function<void()>;
    using PublishCallback = std::function<bool()>;
    using FocusPredicate = std::function<bool()>;

    MacFileClipboard(PlankTransportNativeEndpoint* endpoint,
                     std::string mode,
                     ReadyCallback readyCallback,
                     PublishCallback publishCallback,
                     FocusPredicate focusPredicate = [] { return true; });
    ~MacFileClipboard();

    void chooseFilesOnMainThread();
    bool sendFilesOnMainThread(const std::vector<std::string>& paths);
    std::string status();
    void cancelPendingPasteOnMainThread();
    bool takeReadyPasteOnMainThread();
    void start();
    void stop();
    void pollLocalClipboardOnMainThread();

    /**
     * Consume a remote paste gesture when stable local file URLs are present.
     * The withheld paste is released through ReadyCallback only after the Host
     * has staged and published the files.
     */
    bool beginPasteOnMainThread();
    /** Publish a completed Host transfer to the general pasteboard. */
    bool publishPendingHostFilesOnMainThread();

private:
    struct PasteboardSnapshot {
        std::int64_t changeCount = -1;
        std::uint64_t pasteEpoch = 0;
        bool pasteWhenReady = true;
        std::vector<std::string> paths;
    };

    void setStatus(std::string value);
    void workerLoop();
    enum class SourceResult { Failed, Ready, Yielded };
    SourceResult transferSnapshot(const PasteboardSnapshot& snapshot,
                                  std::vector<std::uint8_t>& yieldedOffer);
    bool receiveHostOffer(std::vector<std::uint8_t> firstRecord);
    bool clientToHostAllowed() const;
    bool hostToClientAllowed() const;

    PlankTransportNativeEndpoint* m_Endpoint;
    std::string m_Mode;
    ReadyCallback m_ReadyCallback;
    PublishCallback m_PublishCallback;
    FocusPredicate m_FocusPredicate;
    std::atomic<std::int64_t> m_PublishedChangeCount = -1;
    std::atomic<std::int64_t> m_StagedChangeCount = -1;
    std::atomic<std::int64_t> m_HostClipboardChangeCount = -1;
    std::uint64_t m_PasteEpoch = 0;
    bool m_ReadyPaste = false;
    std::string m_Status;
    std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::thread m_Worker;
    std::atomic_bool m_Stop {false};
    bool m_Running = false;
    bool m_PasteRequested = false;
    PasteboardSnapshot m_Current;
    PasteboardSnapshot m_Requested;
    std::vector<std::string> m_PendingHostPaths;
    bool m_HostPublishPending = false;
    bool m_HostPublishComplete = false;
    bool m_HostPublishSuccess = false;
};
