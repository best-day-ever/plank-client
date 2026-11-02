#pragma once

#include <QMutex>
#include <QString>
#include <optional>
#include <utility>

// The broker controller and worker threads share one authentication state.
// Auth changes invalidate every outstanding lease and host-refresh ticket.
class BrokerSessionState
{
public:
    struct RefreshTicket {
        quint64 authentication = 0;
        quint64 request = 0;
    };
    QString get() const { QMutexLocker lock(&m_Lock); return m_Token; }
    void set(QString token, bool bound = false) {
        QMutexLocker lock(&m_Lock);
        m_Token = std::move(token);
        m_Bound = bound;
        ++m_Revision;
        ++m_Lease;
        ++m_Refresh;
        m_RefreshInFlight = false;
    }
    void clear() {
        QMutexLocker lock(&m_Lock);
        m_Token.fill(QChar('\0'));
        m_Token.clear();
        m_Bound = false;
        ++m_Revision;
        ++m_Lease;
        ++m_Refresh;
        m_RefreshInFlight = false;
    }
    bool hasToken() const { QMutexLocker lock(&m_Lock); return !m_Token.isEmpty(); }
    bool proofRequired() const { QMutexLocker lock(&m_Lock); return m_Bound; }
    quint64 revision() const { QMutexLocker lock(&m_Lock); return m_Revision; }
    quint64 beginLease(quint64 revision) {
        QMutexLocker lock(&m_Lock);
        return revision == m_Revision && !m_Token.isEmpty() ? ++m_Lease : 0;
    }
    bool currentLease(quint64 revision, quint64 lease) const {
        QMutexLocker lock(&m_Lock);
        return lease != 0 && revision == m_Revision && lease == m_Lease && !m_Token.isEmpty();
    }
    QString getForRevision(quint64 revision) const {
        QMutexLocker lock(&m_Lock);
        return revision == m_Revision ? m_Token : QString();
    }
    void cancelLease() { QMutexLocker lock(&m_Lock); ++m_Lease; }
    std::optional<RefreshTicket> beginRefresh() {
        QMutexLocker lock(&m_Lock);
        if (m_Token.isEmpty() || m_RefreshInFlight) return std::nullopt;
        m_RefreshInFlight = true;
        return RefreshTicket {m_Revision, ++m_Refresh};
    }
    bool finishRefresh(const RefreshTicket& ticket) {
        QMutexLocker lock(&m_Lock);
        if (!m_RefreshInFlight || ticket.authentication != m_Revision || ticket.request != m_Refresh) return false;
        m_RefreshInFlight = false;
        return true;
    }
private:
    mutable QMutex m_Lock;
    QString m_Token;
    bool m_Bound = false;
    quint64 m_Revision = 0;
    quint64 m_Lease = 0;
    quint64 m_Refresh = 0;
    bool m_RefreshInFlight = false;
};
