#pragma once

namespace StationConnectPointerLogic {

inline bool shouldApplyPointerConfinement(bool localToolbarAvailable)
{
    // A native Wayland toolbar is a separate child surface. Constraining the
    // pointer to the SDL parent surface competes with the compositor's focus
    // transition into that child, particularly after renderer or stream-size
    // changes. A fullscreen single-display client is already bounded by the
    // physical screen, so the constraint adds no useful containment there.
    return !localToolbarAvailable;
}

}
