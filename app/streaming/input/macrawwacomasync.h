#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

// HID report callbacks can arrive after a focus/reconnect release. Keep their
// results separate from the Client object and discard an older lease's replies.
class MacWacomAsyncResults
{
public:
    struct Completion {
        std::uint64_t epoch = 0;
        std::uint16_t type = 0, interfaceId = 0, generation = 0;
        std::uint32_t transaction = 0;
        int result = 0;
        std::vector<unsigned char> report;
    };

    std::uint64_t epoch() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Epoch;
    }

    void invalidate()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        ++m_Epoch;
        m_Ready.clear();
    }

    void publish(Completion completion)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (completion.epoch == m_Epoch && m_Ready.size() < 64)
            m_Ready.push_back(std::move(completion));
    }

    std::deque<Completion> take()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::deque<Completion> ready;
        ready.swap(m_Ready);
        return ready;
    }

private:
    mutable std::mutex m_Mutex;
    std::uint64_t m_Epoch = 1;
    std::deque<Completion> m_Ready;
};

// Requested state survives a bounded UI wait. Only the worker acknowledges
// physical release; no input may resume before every requested release is done.
// State and release tickets share a lock so a late completion cannot override
// newer focus/reconnect/shutdown requests.
class MacWacomLifecycle
{
public:
    std::uint64_t setActive(bool active)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping || m_Exited) return 0;
        m_ActiveRequested = active;
        return active ? 0 : ++m_Requested;
    }

    std::uint64_t beginReconnect()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping || m_Exited) return 0;
        m_Reconnecting = true;
        return ++m_Requested;
    }

    std::uint64_t finishReconnect()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Stopping || m_Exited) return 0;
        m_Reconnecting = false;
        return ++m_Requested;
    }

    std::uint64_t stop()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stopping = true;
        m_ActiveRequested = false;
        return ++m_Requested;
    }

    std::uint64_t pendingTicket() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Requested > m_Completed ? m_Requested : 0;
    }

    bool canForward() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_ActiveRequested && !m_Reconnecting && !m_Stopping &&
            !m_Exited && m_Requested == m_Completed;
    }

    bool wait(std::uint64_t ticket, std::chrono::milliseconds deadline)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Completed >= ticket || m_Exited ||
            m_Changed.wait_for(lock, deadline,
                [this, ticket] { return m_Completed >= ticket || m_Exited; });
    }

    void complete(std::uint64_t ticket)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (ticket > m_Completed) m_Completed = ticket;
        m_Changed.notify_all();
    }

    void markExited()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Exited = true;
        m_Changed.notify_all();
    }

    bool waitExited(std::chrono::milliseconds deadline)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        return m_Exited || m_Changed.wait_for(lock, deadline,
            [this] { return m_Exited; });
    }

private:
    mutable std::mutex m_Mutex;
    std::condition_variable m_Changed;
    std::uint64_t m_Requested = 0, m_Completed = 0;
    bool m_ActiveRequested = false, m_Reconnecting = false,
        m_Stopping = false, m_Exited = false;
};
