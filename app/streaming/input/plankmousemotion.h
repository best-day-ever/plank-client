#pragma once

#include <SDL3/SDL.h>

namespace PlankMouseMotion {

// Called only by the SDL event consumer. Preserve button, key, focus and
// cross-window ordering by consuming only adjacent motion from this device.
inline void coalescePending(SDL_MouseMotionEvent& motion)
{
    SDL_Event next;
    while (SDL_PeepEvents(&next, 1, SDL_PEEKEVENT,
                          SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        if (next.type != SDL_EVENT_MOUSE_MOTION ||
                next.motion.windowID != motion.windowID ||
                next.motion.which != motion.which ||
                next.motion.state != motion.state) {
            break;
        }
        if (SDL_PeepEvents(&next, 1, SDL_GETEVENT,
                           SDL_EVENT_FIRST, SDL_EVENT_LAST) <= 0) {
            break;
        }
        motion.timestamp = next.motion.timestamp;
        motion.x = next.motion.x;
        motion.y = next.motion.y;
        motion.xrel += next.motion.xrel;
        motion.yrel += next.motion.yrel;
    }
}

} // namespace PlankMouseMotion
