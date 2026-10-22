#pragma once

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QEventLoop>
#include <QObject>
#include <QQmlEngine>
#include <functional>
#include <utility>

// The GUI keeps servicing the screen setup dialog while SDL owns its worker.
// QML may drop its Session reference during cleanup, but ownership and posted
// deletion stay parked until the caller has joined that worker.
class SessionGuiDispatch : public QObject
{
public:
    SessionGuiDispatch(QObject* session, QObject* application, std::function<void()> cancel)
        : m_Session(session), m_Application(application), m_Cancel(std::move(cancel)),
          m_Ownership(QQmlEngine::objectOwnership(session))
    {
        QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);
        session->installEventFilter(this);
        application->installEventFilter(this);
    }

    ~SessionGuiDispatch() override
    {
        m_Application->removeEventFilter(this);
        m_Session->removeEventFilter(this);
        QQmlEngine::setObjectOwnership(m_Session, m_Ownership);
        if (m_DeletePending) {
            // The filtered event already set QObject's deleteLater latch.
            // Queue a context-bound deletion instead of posting it a second time.
            QObject* session = m_Session;
            QMetaObject::invokeMethod(session, [session] { delete session; }, Qt::QueuedConnection);
        } else if (m_Ready && m_Ownership == QQmlEngine::JavaScriptOwnership)
            m_Session->deleteLater();
    }

    void readyForDeletion() { m_Ready = true; }

    static void dispatch()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, QDeadlineTimer(10));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_Session && event->type() == QEvent::DeferredDelete) {
            m_DeletePending = true;
            cancel();
            return true;
        }
        if (watched == m_Application && event->type() == QEvent::Quit) cancel();
        return false;
    }

private:
    void cancel()
    {
        if (m_Cancelled) return;
        m_Cancelled = true;
        m_Cancel();
    }
    QObject* m_Session;
    QObject* m_Application;
    std::function<void()> m_Cancel;
    QQmlEngine::ObjectOwnership m_Ownership;
    bool m_Ready = false;
    bool m_DeletePending = false;
    bool m_Cancelled = false;
};
