#pragma once

#include "outputtopology.h"
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

// A value-only handoff between the GUI's QScreen inventory and the SDL
// session thread. The GUI services explicit requests while waiting for SDL;
// neither QScreen nor a callable touching it crosses into the worker.
class GuiDisplaySnapshot
{
public:
    void publish(const QVector<NvClientDisplay>& displays)
    {
        QMutexLocker lock(&m_Mutex);
        m_Displays = displays;
        m_Pending = false;
        m_Ready.wakeAll();
    }

    QVector<NvClientDisplay> current() const
    {
        QMutexLocker lock(&m_Mutex);
        return m_Displays;
    }

    QVector<NvClientDisplay> request()
    {
        QMutexLocker lock(&m_Mutex);
        if (m_Cancelled) return {};
        m_Pending = true;
        while (m_Pending && !m_Cancelled) m_Ready.wait(&m_Mutex);
        return m_Cancelled ? QVector<NvClientDisplay>() : m_Displays;
    }

    bool requested() const
    {
        QMutexLocker lock(&m_Mutex);
        return m_Pending;
    }

    void cancel()
    {
        QMutexLocker lock(&m_Mutex);
        m_Cancelled = true;
        m_Pending = false;
        m_Ready.wakeAll();
    }

private:
    mutable QMutex m_Mutex;
    QWaitCondition m_Ready;
    QVector<NvClientDisplay> m_Displays;
    bool m_Pending = false;
    bool m_Cancelled = false;
};
