#include "input.h"
#include "stationconnectpointerlogic.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include "streaming/streamutils.h"

void SdlInputHandler::handleMouseButtonEvent(SDL_MouseButtonEvent* event)
{
    int button;

    if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }
    else if (!isCaptureActive()) {
        if (event->button == SDL_BUTTON_LEFT && !event->down &&
                isMouseInVideoRegion(event->x, event->y)) {
            // Capture the mouse again if clicked when unbound.
            // We start capture on left button released instead of
            // pressed to avoid sending an errant mouse button released
            // event to the host when clicking into our window (since
            // the pressed event was consumed by this code).
            setCaptureActive(true);
        }

        // Not capturing
        return;
    }
    else if (!isMouseInVideoRegion(event->x, event->y) && event->down) {
        // Ignore button presses outside the video region, but allow button releases
        return;
    }

    switch (event->button)
    {
        case SDL_BUTTON_LEFT:
            button = BUTTON_LEFT;
            break;
        case SDL_BUTTON_MIDDLE:
            button = BUTTON_MIDDLE;
            break;
        case SDL_BUTTON_RIGHT:
            button = BUTTON_RIGHT;
            break;
        case SDL_BUTTON_X1:
            button = BUTTON_X1;
            break;
        case SDL_BUTTON_X2:
            button = BUTTON_X2;
            break;
        default:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Unhandled button event: %d",
                        event->button);
            return;
    }

    LiSendMouseButtonEvent(event->down ?
                               BUTTON_ACTION_PRESS :
                               BUTTON_ACTION_RELEASE,
                           button);
}

void SdlInputHandler::handleMouseMotionEvent(SDL_MouseMotionEvent* event,
                                             bool batchPendingEvents)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    // Batch all pending mouse motion events to save CPU time
    Sint32 x = event->x, y = event->y;
    SDL_Event nextEvent;
    while (batchPendingEvents &&
           SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT,
                          SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION) > 0) {
        event = &nextEvent.motion;

        // Ignore synthetic mouse events
        if (event->which != SDL_TOUCH_MOUSEID) {
            x = event->x;
            y = event->y;
        }
    }

    // We should not reference the original event anymore
    event = nullptr;

    int windowWidth, windowHeight;
    SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);

    SDL_Rect src, dst;
    bool mouseInVideoRegion;

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Use the stream and window sizes to determine the video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    mouseInVideoRegion = isMouseInVideoRegion(x, y, windowWidth, windowHeight);

    // Clamp motion to the video region
    x = qMin(qMax(x - dst.x, 0), dst.w);
    y = qMin(qMax(y - dst.y, 0), dst.h);

    // Send the mouse position update if one of the following is true:
    // a) it is in the video region now
    // b) it just left the video region (to ensure the mouse is clamped to the video boundary)
    // c) a mouse button is still down from before the cursor left the video region (to allow smooth dragging)
    Uint32 buttonState = SDL_GetMouseState(nullptr, nullptr);
    if (buttonState == 0) {
        if (m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
            if (m_NeedsManualCaptureOnLeave) {
                SDL_CaptureMouse(false);
            }
            m_PendingMouseButtonsAllUpOnVideoRegionLeave = false;
        }
    }
    if (mouseInVideoRegion || m_MouseWasInVideoRegion || m_PendingMouseButtonsAllUpOnVideoRegionLeave) {
        LiSendMousePositionEvent((short)x, (short)y, dst.w, dst.h);
    }

    // Adjust the cursor visibility if applicable
    if (mouseInVideoRegion ^ m_MouseWasInVideoRegion) {
        setCursorVisible(!mouseInVideoRegion ||
                         m_MouseCursorCapturedVisibilityState);
        if (!mouseInVideoRegion && buttonState != 0) {
            // If we still have a button pressed on leave, wait for that to come up
            // before we stop sending mouse position events.
            m_PendingMouseButtonsAllUpOnVideoRegionLeave = true;
        }
    }

    m_MouseWasInVideoRegion = mouseInVideoRegion;
}

void SdlInputHandler::handleMouseWheelEvent(SDL_MouseWheelEvent* event)
{
    if (!isCaptureActive()) {
        // Not capturing
        return;
    }
    else if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }

    const int mouseX = qRound(event->mouse_x);
    const int mouseY = qRound(event->mouse_y);
    if (!isMouseInVideoRegion(mouseX, mouseY)) {
        // Ignore scroll events outside the video region
        return;
    }

    if (event->y != 0.0f) {
#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->y = SDL_clamp(event->y, -1.0f, 1.0f);
#endif

        LiSendHighResScrollEvent((short)(event->y * 120)); // WHEEL_DELTA
    }

    if (event->x != 0.0f) {
#ifdef Q_OS_DARWIN
        // HACK: Clamp the scroll values on macOS to prevent OS scroll acceleration
        // from generating wild scroll deltas when scrolling quickly.
        event->x = SDL_clamp(event->x, -1.0f, 1.0f);
#endif

        LiSendHighResHScrollEvent((short)(event->x * 120)); // WHEEL_DELTA
    }
}

bool SdlInputHandler::isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth, int windowHeight)
{
    SDL_Rect src, dst;

    if (windowWidth < 0 || windowHeight < 0) {
        SDL_GetWindowSize(m_Window, &windowWidth, &windowHeight);
    }

    src.x = src.y = 0;
    src.w = m_StreamWidth;
    src.h = m_StreamHeight;

    dst.x = dst.y = 0;
    dst.w = windowWidth;
    dst.h = windowHeight;

    // Use the stream and window sizes to determine the video region
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    return (mouseX >= dst.x && mouseX <= dst.x + dst.w) &&
           (mouseY >= dst.y && mouseY <= dst.y + dst.h);
}

void SdlInputHandler::updatePointerRegionLock()
{
    if (m_Window == nullptr) {
        return;
    }

    // Our pointer lock behavior tracks with the fullscreen mode unless the user has
    // toggled it themselves using the keyboard shortcut. If that's the case, they
    // have full control over it and we don't touch it anymore.
    if (!m_PointerRegionLockToggledByUser) {
        // Lock the pointer in true full-screen mode or in any fullscreen mode when only a single monitor is present
        const bool fullscreen = (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
        m_PointerRegionLockActive = fullscreen && StreamUtils::getDisplayCount() == 1;
    }

    // If region lock is enabled, grab the cursor so it can't accidentally leave our window.
    if (isCaptureActive() && m_PointerRegionLockActive &&
            StationConnectPointerLogic::shouldApplyPointerConfinement(
                m_LocalToolbarAvailable)) {
        SDL_Rect src, videoRect;

        src.x = src.y = 0;
        src.w = m_StreamWidth;
        src.h = m_StreamHeight;

        videoRect.x = videoRect.y = 0;
        SDL_GetWindowSize(m_Window, &videoRect.w, &videoRect.h);
        // Use the stream and window sizes to determine the video region.
        StreamUtils::scaleSourceToDestinationSurface(&src, &videoRect);
        SDL_SetWindowMouseRect(m_Window, &videoRect);
    }
    else {
        // Allow the cursor to leave the bounds of our video region or window
        SDL_SetWindowMouseRect(m_Window, nullptr);
    }
}
