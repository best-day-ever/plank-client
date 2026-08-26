#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LinuxRawWacomInput
{
public:
    LinuxRawWacomInput();
    ~LinuxRawWacomInput();

    LinuxRawWacomInput(const LinuxRawWacomInput&) = delete;
    LinuxRawWacomInput& operator=(const LinuxRawWacomInput&) = delete;

    void setActive(bool active);
    void beginReconnect();
    void finishReconnect();
    void handleControl(const unsigned char* data, unsigned int length);

private:
    struct HidInterface {
        int fd;
        std::vector<unsigned char> descriptor;
    };

    void run();
    bool discover();
    bool sendAttach();
    bool sendFrame(std::uint16_t type, std::uint16_t interfaceId,
                   std::uint32_t transactionId,
                   const unsigned char* payload, std::size_t payloadLength);
    void handlePhysicalReports();
    void handleGetReport(std::uint16_t interfaceId, std::uint32_t transactionId,
                         const unsigned char* payload, std::size_t payloadLength);
    void handleSetReport(std::uint16_t type, std::uint16_t interfaceId,
                         std::uint32_t transactionId,
                         const unsigned char* payload, std::size_t payloadLength);
    void setGrabbed(bool grabbed);
    void suspendForFocusLoss();
    void release(bool notifyHost);

    std::atomic<bool> m_Active;
    std::atomic<bool> m_Stopping;
    std::atomic<bool> m_Reconnecting;
    std::atomic<bool> m_AttachFailed;
    std::thread m_Thread;
    std::recursive_mutex m_Mutex;
    std::vector<HidInterface> m_Interfaces;
    std::vector<int> m_EventFds;
    std::uint16_t m_Generation;
    std::uint32_t m_InputSequence;
    bool m_AttachPending;
    bool m_Attached;
    std::chrono::steady_clock::time_point m_AttachDeadline;
};
