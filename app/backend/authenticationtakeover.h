#pragma once

#include <QDeadlineTimer>
#include <QMutex>
#include <QSharedPointer>
#include <QWaitCondition>
#include <QMetaType>
#include <QJsonObject>
#include <QUuid>

inline QString macActiveSessionId(int status, const QJsonObject& reply)
{
    if (status != 409 || reply.value(QStringLiteral("state")) != QLatin1String("conflict") ||
            reply.value(QStringLiteral("error")) != QLatin1String("session_active")) return {};
    const QString id = reply.value(QStringLiteral("session_id")).toString();
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id ? id : QString();
}

// The authentication worker may wait; the GUI never does. Shared ownership
// makes Cancel, timeout, shutdown and a late button click safe in either order.
class AuthenticationTakeoverDecision
{
public:
    void respond(bool accepted) {
        QMutexLocker lock(&m_Mutex);
        if (m_Decided) return;
        m_Decided = true;
        m_Accepted = accepted;
        m_Ready.wakeAll();
    }
    bool wait(int timeoutMs = 120000) {
        QMutexLocker lock(&m_Mutex);
        const QDeadlineTimer deadline(timeoutMs);
        while (!m_Decided && m_Ready.wait(&m_Mutex, deadline)) {}
        m_Decided = true;
        return m_Accepted;
    }
private:
    QMutex m_Mutex;
    QWaitCondition m_Ready;
    bool m_Decided = false;
    bool m_Accepted = false;
};
using AuthenticationTakeover = QSharedPointer<AuthenticationTakeoverDecision>;
Q_DECLARE_METATYPE(AuthenticationTakeover)
