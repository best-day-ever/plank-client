#pragma once

#include <atomic>
#include <cstdint>

// One deadline shared by the UI and reconnect worker. Requests already in
// flight remain bounded by their own HTTP timeout; no new request may begin
// while the local Ask prompt awaits a decision.
class PlankReconnectPolicy
{
public:
    void allowUntil(uint64_t deadline) { m_Deadline.store(deadline); }
    bool allowsRequest(uint64_t now) const { return now < m_Deadline.load(); }

    static bool terminalStatus(int status, bool authenticating)
    {
        return status == 403 || status == 423 ||
                (authenticating && status == 401) ||
                (status >= 400 && status < 500 && status != 401 &&
                 status != 409 && status != 425 && status != 429);
    }

private:
    std::atomic<uint64_t> m_Deadline {0};
};
