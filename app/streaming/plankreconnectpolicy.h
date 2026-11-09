#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

// The frontend chooses bounded or continuous recovery. Individual operations
// retain their HTTP timeout, cancellation and terminal authentication checks.
// The ordinary frontend's Ask policy still pauses requests at its deadline.
class PlankReconnectPolicy
{
public:
    void allowUntil(uint64_t deadline) { m_Deadline.store(deadline); }
    void allowContinuously() { allowUntil(std::numeric_limits<uint64_t>::max()); }
    bool allowsRequest(uint64_t now) const { return now < m_Deadline.load(); }

    static bool terminalStatus(int status, bool authenticating,
                               bool openingDesktop = false)
    {
        return status == 403 || status == 423 ||
                (authenticating && status == 401 && !openingDesktop) ||
                (status >= 400 && status < 500 && status != 401 &&
                 status != 409 && status != 425 && status != 429);
    }

private:
    std::atomic<uint64_t> m_Deadline {0};
};

// A launch accepted a display transition, so its worker and bearer sessions
// may disappear before the layout is ready. Keep that recovery inside the
// original layout deadline, with a shorter grace for admission denials. The
// grace starts at the first lost worker token and never slides on retries.
class PlankDisplayTransitionPolicy
{
public:
    explicit PlankDisplayTransitionPolicy(uint64_t maximumWaitMs) :
        m_MaximumWaitMs(maximumWaitMs) {}

    bool expired(uint64_t elapsedMs) const { return elapsedMs >= m_MaximumWaitMs; }
    bool needsAuthentication() const { return m_NeedsAuthentication; }

    void authenticationLost(uint64_t elapsedMs)
    {
        if (!m_AuthenticationLost) {
            m_AuthenticationLost = true;
            m_AuthenticationLostAt = elapsedMs;
        }
        m_NeedsAuthentication = true;
    }

    void authenticated() { m_NeedsAuthentication = false; }

    bool retryAuthentication(int status, uint64_t elapsedMs) const
    {
        const bool replacingWorker = m_AuthenticationLost &&
                elapsedMs >= m_AuthenticationLostAt &&
                elapsedMs - m_AuthenticationLostAt < 15000;
        return m_NeedsAuthentication && !expired(elapsedMs) &&
                !PlankReconnectPolicy::terminalStatus(status, true, replacingWorker);
    }

private:
    const uint64_t m_MaximumWaitMs;
    uint64_t m_AuthenticationLostAt = 0;
    bool m_AuthenticationLost = false;
    bool m_NeedsAuthentication = false;
};
