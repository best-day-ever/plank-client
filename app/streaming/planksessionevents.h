#pragma once

#include <SDL3/SDL_events.h>

namespace PlankSessionEvents {
// SDL emits Quit only for the last visible top-level window. A stream can
// own several windows, so each owned Close request must end the session.
inline bool closeRequestsDisconnect(const SDL_Event& event, bool isPresentationWindow)
{
    return event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && isPresentationWindow;
}
}
