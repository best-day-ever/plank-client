#pragma once

// Shared by every normalized-pen client path (Linux libinput today,
// macOS SDL3 pen events as of this file) that needs to put a pen's tilt
// onto the PLANK_TRANSPORT_INPUT_PEN wire packet
// (plank_transport_input_encode_pen(), protocol/plank-transport/include/plank_transport_input.h).
//
// The wire format has no independent X/Y tilt fields. It carries a single
// combined magnitude/direction pair instead: `tilt` is the angle away from
// vertical in degrees (0..90) and `rotation` is the compass direction that
// tilt leans toward, in degrees (0..359, 0 facing up). Both libinput's
// tilt_x/tilt_y and SDL's SDL_PEN_AXIS_XTILT/SDL_PEN_AXIS_YTILT report the
// same underlying quantity: two independent bidirectional angles in
// degrees (-90..90), so the same trigonometric transform applies to both.
//
// The host-side inverse transform lives in
// apps/host/linux/src/platform/virtualhid_input.cpp (pen_update()); keep
// that function and this one in sync if either changes.
//
// This header intentionally has no platform dependency (no libinput, no
// SDL, no Qt) so it can be included directly by a plain unit test.

#include <algorithm>
#include <cmath>

inline void plankEncodePenTilt(double tiltXDegrees, double tiltYDegrees,
                                unsigned short& rotation, unsigned char& tilt)
{
    const double pi = std::acos(-1.0);
    const double x = std::tan(tiltXDegrees * pi / 180.0);
    const double y = std::tan(tiltYDegrees * pi / 180.0);
    const double magnitude = std::atan(std::hypot(x, y)) * 180.0 / pi;
    double direction = -std::atan2(x, y) * 180.0 / pi;
    if (direction < 0.0) {
        direction += 360.0;
    }

    tilt = static_cast<unsigned char>(std::lround(
        std::max(0.0, std::min(90.0, magnitude))));
    rotation = static_cast<unsigned short>(std::lround(direction)) % 360;
}
