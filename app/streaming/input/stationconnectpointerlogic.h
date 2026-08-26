#pragma once

namespace StationConnectPointerLogic {

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
