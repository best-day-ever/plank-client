#pragma once

#include <cstdint>

namespace PlankPointerLogic {

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

inline bool tabletFocusPositionIsCurrent(bool tabletActive, bool positionValid,
                                         std::uint64_t positionSequence,
                                         std::uint64_t activationSequence)
{
    // A delayed Host position must not reclaim focus after mouse input or use
    // the previous capture/connection's position when tablet input resumes.
    return tabletActive && positionValid && positionSequence > activationSequence;
}

}
