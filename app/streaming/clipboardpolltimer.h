#pragma once

#include <SDL3/SDL.h>
#include <cstdint>

// The callback carries only an event code, never a Session pointer. Queued
// events are harmless after teardown because the clipboard sync is stopped.
class ClipboardPollTimer
{
public:
    ClipboardPollTimer() = default;
    ClipboardPollTimer(const ClipboardPollTimer&) = delete;
    ClipboardPollTimer& operator=(const ClipboardPollTimer&) = delete;
    ~ClipboardPollTimer() { stop(); }

    bool start(Sint32 eventCode)
    {
        if (m_TimerId != 0) {
            return true;
        }
        m_TimerId = SDL_AddTimer(250, callback,
                    reinterpret_cast<void*>(static_cast<std::intptr_t>(eventCode)));
        if (m_TimerId == 0) {
            return false;
        }
        queue(eventCode);
        return true;
    }

    void stop()
    {
        if (m_TimerId != 0) {
            SDL_RemoveTimer(m_TimerId);
            m_TimerId = 0;
        }
    }

    static void queue(Sint32 eventCode)
    {
        SDL_Event event {};
        event.type = SDL_EVENT_USER;
        event.user.code = eventCode;
        event.user.timestamp = SDL_GetTicks();
        SDL_PushEvent(&event);
    }

private:
    static Uint32 callback(void* userdata, SDL_TimerID, Uint32 interval)
    {
        queue(static_cast<Sint32>(reinterpret_cast<std::intptr_t>(userdata)));
        return interval;
    }

    SDL_TimerID m_TimerId = 0;
};
