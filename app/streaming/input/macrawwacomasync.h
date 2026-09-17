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

// The Client waits for physical release, but a wedged device must not hold the
// event loop forever. The worker owns its state after a timed-out Quit.
class MacWacomReleaseBarrier
{
public:
    std::uint64_t request()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return ++m_Requested;
    }

    std::uint64_t pendingTicket() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Requested > m_Completed ? m_Requested : 0;
    }

    bool idle() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Requested == m_Completed && !m_Exited;
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
    bool m_Exited = false;
};
