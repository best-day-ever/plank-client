#pragma once

// macOS normalized-pen client forwarding.
//
// SDL3's Cocoa backend translates AppKit tablet events
// (NSEventTypeTabletPoint / NSEventTypeTabletProximity, carrying pressure,
// tilt, rotation and barrel buttons) into SDL's own cross-platform pen API
// (SDL_EVENT_PEN_PROXIMITY_IN/OUT, SDL_EVENT_PEN_DOWN/UP,
// SDL_EVENT_PEN_BUTTON_DOWN/UP, SDL_EVENT_PEN_MOTION, SDL_EVENT_PEN_AXIS;
// see SDL3/SDL_pen.h). That means no Cocoa/IOKit code is needed here at
// all: this class only ever sees primitive values extracted from those SDL
// events by streaming/input/input.cpp, and turns them into the same
// LiSendPenEvent()/PLANK_TRANSPORT_INPUT_PEN ("type7") normalized-pen wire
// packets that the Linux client's LinuxWacomInput
// (streaming/input/linuxwacom.cpp) already sends over the libinput path.
// See docs/bde/gap-analysis.md and protocol/wacom-hid.md for the wider
// picture, and docs/bde/mac-pen-testplan.md for the manual hardware test
// plan (no Wacom hardware was available to exercise this in development).
//
// Unlike LinuxWacomInput, this class owns no thread and grabs no device
// node: SDL already delivers pen events on the same event loop as mouse
// and keyboard input, and macOS raw HID tablet forwarding
// (protocol/wacom-hid.md) remains out of scope for this pass -- only the
// normalized-pen fallback path is implemented for macOS here.
//
// This header has no SDL/Qt/AppKit dependency -- only <Limelight.h>, which
// is a plain C header -- so its translation logic can be exercised by a
// plain unit test with no session, window, or transport brought up
// (tests/macpen/test_macpen.cpp mirrors tests/wacomtransportpolicy).

#include "pentiltencoding.h"

#include <Limelight.h>

#include <cstdint>
#include <functional>

// Mirrors the SDL_PenInputFlags bits (SDL3/SDL_pen.h) actually consumed
// here. Duplicated as plain constants rather than including SDL3/SDL_pen.h
// so this header stays SDL-free; streaming/input/input.cpp (which does
// include SDL3/SDL.h) is responsible for keeping these in sync with the
// real SDL_PEN_INPUT_* values when it extracts pen_state from an SDL_Event.
namespace PlankMacPenInputFlags {
constexpr std::uint32_t Button1 = 1u << 1;
constexpr std::uint32_t Button2 = 1u << 2;
constexpr std::uint32_t Button3 = 1u << 3;
constexpr std::uint32_t EraserTip = 1u << 30;
} // namespace PlankMacPenInputFlags

// Mirrors the ordinal values of the SDL_PenAxis enum (SDL3/SDL_pen.h).
enum class PlankMacPenAxis {
    Pressure = 0,
    XTilt = 1,
    YTilt = 2,
    Distance = 3,
    Rotation = 4,
    Slider = 5,
    TangentialPressure = 6,
};

// One LiSendPenEvent()-shaped packet, ready to forward. Kept as a plain
// struct -- rather than calling LiSendPenEvent() directly from inside the
// translation logic below -- so that logic can be exercised without
// linking moonlight-common-c's InputStream.c or bringing up a session.
struct PlankMacPenPacket {
    std::uint8_t eventType;
    std::uint8_t toolType;
    std::uint8_t buttons;
    float x;
    float y;
    float pressureOrDistance;
    std::uint16_t rotation;
    std::uint8_t tilt;
};

// Translates a sequence of SDL pen events (already reduced to primitive
// coordinates/flags by the caller) into PlankMacPenPacket values, matching
// LinuxWacomInput's semantics as closely as the two input models allow.
//
// The PLANK_TRANSPORT_INPUT_PEN wire packet has no per-pen identifier, so
// (like LinuxWacomInput) only one logical pen is ever represented on the
// wire at a time. If a second physical pen (a different SDL_PenID) starts
// producing events while the first is still in proximity, that is treated
// as a tool swap: the old pen is released (LI_TOUCH_EVENT_HOVER_LEAVE)
// before the new one's events are forwarded, mirroring how the macOS
// host-side consumer of this same wire format is documented to handle a
// tool swap ("Tool change releases old contact and exits proximity before
// entering the new tool", protocol/macos-pen-input.md).
class PlankMacPenInput {
public:
    using Sink = std::function<void(const PlankMacPenPacket&)>;
    using ActivityCallback = std::function<void()>;

    explicit PlankMacPenInput(Sink sink, ActivityCallback activity = nullptr)
        : m_Sink(std::move(sink)), m_Activity(std::move(activity))
    {
    }

    // Mirrors LinuxWacomInput::setActive(): while inactive, no packets are
    // sent (matches the window losing input focus). Deactivating an
    // in-proximity pen sends one LI_TOUCH_EVENT_CANCEL_ALL first, matching
    // LinuxWacomInput::cancelRemotePen().
    void setActive(bool active)
    {
        if (m_Active == active) {
            return;
        }
        if (!active) {
            // Send the cancellation while still marked active so send()
            // below actually emits it, then deactivate.
            cancelActivePen();
        }
        m_Active = active;
    }

    // SDL_EVENT_PEN_PROXIMITY_IN. Carries no coordinate (SDL_pen.h: proximity
    // events fire before any motion/axis event establishes a position), so
    // nothing is sent yet; the first position-bearing event does that.
    void handleProximityIn(std::uint32_t penId)
    {
        beginOrContinuePen(penId);
    }

    // SDL_EVENT_PEN_PROXIMITY_OUT.
    void handleProximityOut(std::uint32_t penId)
    {
        if (!m_HavePen || penId != m_CurrentPenId) {
            return;
        }
        send(LI_TOUCH_EVENT_HOVER_LEAVE);
        m_HavePen = false;
        m_State = PenState();
    }

    // SDL_EVENT_PEN_MOTION. x/y are normalized device coordinates in
    // 0.0..1.0 covering the video area, already mapped from window
    // coordinates by the caller (see
    // PlankPresentation::mapWindowPointToStream(), used the same way for
    // absolute mouse positioning in streaming/input/mouse.cpp).
    void handlePosition(std::uint32_t penId, float normalizedX,
                         float normalizedY, std::uint32_t penInputFlags)
    {
        beginOrContinuePen(penId);
        m_State.x = clamp01(normalizedX);
        m_State.y = clamp01(normalizedY);
        applyFlags(penInputFlags);
        send(m_State.tipDown ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_HOVER);
    }

    // SDL_EVENT_PEN_DOWN / SDL_EVENT_PEN_UP.
    void handleTouch(std::uint32_t penId, bool down, float normalizedX,
                      float normalizedY, std::uint32_t penInputFlags)
    {
        beginOrContinuePen(penId);
        m_State.x = clamp01(normalizedX);
        m_State.y = clamp01(normalizedY);
        applyFlags(penInputFlags);
        m_State.tipDown = down;
        send(down ? LI_TOUCH_EVENT_DOWN : LI_TOUCH_EVENT_UP);
    }

    // SDL_EVENT_PEN_BUTTON_DOWN / SDL_EVENT_PEN_BUTTON_UP. x, y, pressure,
    // tilt and rotation are documented (Limelight.h) to be ignored
    // host-side for LI_TOUCH_EVENT_BUTTON_ONLY, so only the buttons mask
    // needs to be current.
    void handleButton(std::uint32_t penId, std::uint32_t penInputFlags)
    {
        beginOrContinuePen(penId);
        applyFlags(penInputFlags);
        send(LI_TOUCH_EVENT_BUTTON_ONLY);
    }

    // SDL_EVENT_PEN_AXIS. `value` is in the units SDL_pen.h documents for
    // `axis` (0.0..1.0 for pressure/distance, degrees for tilt).
    //
    // SDL_PEN_AXIS_ROTATION reports true barrel rotation (e.g. an Art
    // Pen's twist), which is a different quantity from the wire format's
    // "rotation" field -- that field is the *direction* of the combined
    // X/Y tilt vector (see pentiltencoding.h), derived only from XTilt/
    // YTilt below, exactly as LinuxWacomInput derives it from libinput's
    // tilt_x/tilt_y. SDL_PEN_AXIS_SLIDER and
    // SDL_PEN_AXIS_TANGENTIAL_PRESSURE have no wire representation at all.
    // All three are therefore intentionally ignored here.
    void handleAxis(std::uint32_t penId, PlankMacPenAxis axis, float value)
    {
        beginOrContinuePen(penId);
        switch (axis) {
        case PlankMacPenAxis::Pressure:
            m_State.pressure = clamp01(value);
            break;
        case PlankMacPenAxis::Distance:
            m_State.distance = clamp01(value);
            break;
        case PlankMacPenAxis::XTilt:
            m_State.haveXTilt = true;
            m_State.tiltXDegrees = value;
            break;
        case PlankMacPenAxis::YTilt:
            m_State.haveYTilt = true;
            m_State.tiltYDegrees = value;
            break;
        case PlankMacPenAxis::Rotation:
        case PlankMacPenAxis::Slider:
        case PlankMacPenAxis::TangentialPressure:
            return;
        }
        if (m_State.haveXTilt && m_State.haveYTilt) {
            plankEncodePenTilt(m_State.tiltXDegrees, m_State.tiltYDegrees,
                                m_State.rotation, m_State.tilt);
        }
        send(m_State.tipDown ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_HOVER);
    }

private:
    struct PenState {
        std::uint8_t toolType = LI_TOOL_TYPE_PEN;
        std::uint8_t buttons = 0;
        bool tipDown = false;
        float x = 0.0f;
        float y = 0.0f;
        float pressure = 0.0f;
        float distance = 0.0f;
        bool haveXTilt = false;
        bool haveYTilt = false;
        double tiltXDegrees = 0.0;
        double tiltYDegrees = 0.0;
        std::uint16_t rotation = LI_ROT_UNKNOWN;
        std::uint8_t tilt = LI_TILT_UNKNOWN;
    };

    void beginOrContinuePen(std::uint32_t penId)
    {
        if (m_HavePen && penId != m_CurrentPenId) {
            // A different physical pen took over -- release the old one
            // first so the host never sees two pens' input blended
            // together on this single-pen wire format.
            send(LI_TOUCH_EVENT_HOVER_LEAVE);
            m_State = PenState();
        }
        m_CurrentPenId = penId;
        m_HavePen = true;
    }

    void cancelActivePen()
    {
        if (m_HavePen) {
            send(LI_TOUCH_EVENT_CANCEL_ALL);
        }
        m_HavePen = false;
        m_State = PenState();
    }

    void applyFlags(std::uint32_t flags)
    {
        using namespace PlankMacPenInputFlags;
        m_State.toolType =
            (flags & EraserTip) ? LI_TOOL_TYPE_ERASER : LI_TOOL_TYPE_PEN;
        std::uint8_t buttons = 0;
        if (flags & Button1) {
            buttons |= LI_PEN_BUTTON_PRIMARY;
        }
        if (flags & Button2) {
            buttons |= LI_PEN_BUTTON_SECONDARY;
        }
        if (flags & Button3) {
            buttons |= LI_PEN_BUTTON_TERTIARY;
        }
        m_State.buttons = buttons;
    }

    static float clamp01(float value)
    {
        if (!(value >= 0.0f)) { // also rejects NaN
            return 0.0f;
        }
        return value > 1.0f ? 1.0f : value;
    }

    void send(std::uint8_t eventType)
    {
        if (!m_Active || !m_Sink) {
            return;
        }
        const float pressureOrDistance =
            m_State.tipDown ? m_State.pressure : m_State.distance;
        m_Sink(PlankMacPenPacket {
            eventType, m_State.toolType, m_State.buttons,
            m_State.x, m_State.y, pressureOrDistance,
            m_State.rotation, m_State.tilt,
        });
        if (m_Activity) {
            m_Activity();
        }
    }

    Sink m_Sink;
    ActivityCallback m_Activity;
    bool m_Active = false;
    bool m_HavePen = false;
    std::uint32_t m_CurrentPenId = 0;
    PenState m_State;
};
