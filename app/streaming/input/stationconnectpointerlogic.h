#pragma once

#include <SDL3/SDL.h>

namespace StationConnectPointerLogic {

inline bool isSyntheticMouseDevice(SDL_MouseID device)
{
    return device == SDL_TOUCH_MOUSEID || device == SDL_PEN_MOUSEID;
}

struct Rect
{
    int x;
    int y;
    int w;
    int h;
};

inline Rect pointerConfinementRect(const Rect& windowRect,
                                   const Rect& videoRect,
                                   bool localToolbarAvailable)
{
    return localToolbarAvailable ? windowRect : videoRect;
}

}
