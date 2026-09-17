#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QObject>

#include <SDL3/SDL.h>

// Native macOS Quit is delivered synchronously to Qt even while the session
// runs its SDL event loop on the main thread. Wake that loop too, so it can
// release input, destroy the renderer, and dispatch connection cleanup before
// returning to Qt's pending application exit. SDL-only disconnects still
// return to the host list; they do not request application termination.
class MacQuitBridge final : public QObject
{
public:
    explicit MacQuitBridge(QCoreApplication& application) :
        QObject(&application), m_Application(application)
    {
        m_Application.installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* receiver, QEvent* event) override
    {
        if (receiver == &m_Application && event->type() == QEvent::Quit &&
                SDL_WasInit(SDL_INIT_EVENTS)) {
            SDL_Event quitEvent = {};
            quitEvent.type = SDL_EVENT_QUIT;
            quitEvent.quit.timestamp = SDL_GetTicksNS();
            if (SDL_PushEvent(&quitEvent)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Mac application quit forwarded to the streaming loop");
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unable to forward Mac application quit: %s", SDL_GetError());
            }
        }
        // Keep Qt's normal close/exit handling. main() waits for deferred
        // session cleanup before tearing down the application and logger.
        return QObject::eventFilter(receiver, event);
    }

private:
    QCoreApplication& m_Application;
};
