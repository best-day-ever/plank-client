#pragma once

#include <QEvent>
#include <QGuiApplication>

// macOS delivers native application Quit while SDL owns the main thread.
// Keep Qt alive until every started session has released its native windows
// and completed deferred transport cleanup. Disconnect is not application Quit.
class MacApplication final : public QGuiApplication
{
    Q_OBJECT

public:
    using QGuiApplication::QGuiApplication;

    bool beginSession()
    {
        if (m_ExitRequested) {
            return false;
        }
        ++m_Sessions;
        return true;
    }

    void endSession()
    {
        Q_ASSERT(m_Sessions > 0);
        if (--m_Sessions == 0 && m_ExitRequested) {
            // Never terminate reentrantly inside a session completion signal.
            QCoreApplication::postEvent(this, new QEvent(QEvent::Quit));
        }
    }

signals:
    void exitRequested();

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::Quit && m_Sessions != 0) {
            event->ignore();
            if (!m_ExitRequested) {
                m_ExitRequested = true;
                emit exitRequested();
            }
            return true;
        }
        return QGuiApplication::event(event);
    }

private:
    // A new connection may start while a disconnected session finishes its
    // asynchronous cleanup. Both must finish before the application exits.
    unsigned int m_Sessions = 0;
    bool m_ExitRequested = false;
};
