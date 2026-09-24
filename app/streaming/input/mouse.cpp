#include "input.h"
#include "plankpointerlogic.h"
#include "plankmousemotion.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include "streaming/streamutils.h"

void SdlInputHandler::routePresentationPointer(SDL_Window*& window, float& x, float& y) const
{
#ifdef Q_OS_MACOS
    if (!m_PresentationLayout.isMultiOutput()) return;
    int originX, originY;
    if (!SDL_GetWindowPosition(window, &originX, &originY)) return;
    QVector<QRect> bounds;
    for (const auto& output : m_PresentationLayout.outputs) {
        int left, top, width, height;
        if ((SDL_GetWindowFlags(output.window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0 &&
                SDL_GetWindowPosition(output.window, &left, &top) &&
                SDL_GetWindowSize(output.window, &width, &height)) {
            bounds.append(QRect(left, top, width, height));
        } else {
            bounds.append(QRect());
        }
    }
    QPointF local;
    const int target = PlankPresentation::capturedPointerTarget(
                QPointF(x, y), QPoint(originX, originY), bounds, local);
    if (target >= 0) {
        window = m_PresentationLayout.outputs.at(target).window;
        x = float(local.x());
        y = float(local.y());
    }
#else
    Q_UNUSED(window);
    Q_UNUSED(x);
    Q_UNUSED(y);
#endif
}

void SdlInputHandler::handleMouseButtonEvent(SDL_MouseButtonEvent* event)
{
    int button;
    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    if (event->which == SDL_TOUCH_MOUSEID) {
        // Ignore synthetic mouse events
        return;
    }
    SDL_MouseButtonEvent routedEvent = *event;
    routePresentationPointer(window, routedEvent.x, routedEvent.y);
    routedEvent.windowID = SDL_GetWindowID(window);
    event = &routedEvent;
    activateCompositorCursor();
    if (!isCaptureActive()) {
        if (event->button == SDL_BUTTON_LEFT && !event->down &&
                isMouseInVideoRegion(qRound(x), qRound(y),
                                     SDL_GetWindowID(window))) {
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
    else if (!isMouseInVideoRegion(qRound(x), qRound(y),
                                   SDL_GetWindowID(window)) && event->down) {
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

    // Button packets carry no coordinates. Reassert the SDL button event's
    // absolute position immediately before the button so a stale tablet or
    // coalesced motion sample cannot make the remote click land elsewhere.
    const bool positioned = sendAbsoluteMousePosition(
                window, event->x, event->y, !event->down);
    if (event->down && !positioned) {
        return;
    }

    LiSendMouseButtonEvent(event->down ?
                               BUTTON_ACTION_PRESS :
                               BUTTON_ACTION_RELEASE,
                           button);
    if (!event->down) {
        // A captured drag can finish on the other output without another move.
        // Forward its release before transferring native window focus.
        followPointerFocus(window, 0);
    }
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
    activateCompositorCursor();

    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    // Batch all pending mouse motion events to save CPU time
    // Keep SDL's fractional point coordinates: on a 2x display one point is
    // two drawable pixels, so truncating here would lose half the precision.
    float x = event->x, y = event->y;
    SDL_Event nextEvent;
    while (batchPendingEvents &&
           SDL_PeepEvents(&nextEvent, 1, SDL_GETEVENT,
                          SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION) > 0) {
        event = &nextEvent.motion;

        // Ignore synthetic mouse events
        if (event->which != SDL_TOUCH_MOUSEID &&
                event->windowID == SDL_GetWindowID(window)) {
            x = event->x;
            y = event->y;
        } else if (event->windowID != SDL_GetWindowID(window)) {
            SDL_PushEvent(&nextEvent);
            break;
        }
    }
    float x = motion.x, y = motion.y;

    // We should not reference the original event anymore
    event = nullptr;

    // Cocoa/SDL auto-capture keeps a drag addressed to its starting window.
    // Route the coordinates across our windows without releasing the button
    // or transferring native capture/focus between them.
    routePresentationPointer(window, x, y);

    const bool mouseInVideoRegion = isMouseInVideoRegion(
                x, y, SDL_GetWindowID(window));

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
        sendAbsoluteMousePosition(window, qRound(x), qRound(y), true);
    }

    // Adjust the cursor visibility if applicable
    if (mouseInVideoRegion ^ m_MouseWasInVideoRegion) {
        setCursorVisible(!mouseInVideoRegion ||
                         (m_LocalCursorSupported ? m_RemoteCursorVisible :
                                                   m_MouseCursorCapturedVisibilityState));
        if (!mouseInVideoRegion && buttonState != 0) {
            // If we still have a button pressed on leave, wait for that to come up
            // before we stop sending mouse position events.
            m_PendingMouseButtonsAllUpOnVideoRegionLeave = true;
        }
    }

    m_MouseWasInVideoRegion = mouseInVideoRegion;
    followPointerFocus(window, motion.state);
}

bool SdlInputHandler::mapWindowPointToDesktop(SDL_Window* window, float windowX, float windowY,
                                              QPointF& desktopPoint, bool allowClampedPosition) const
{
    const auto* output = presentationOutput(window);
    if (output == nullptr) {
        return false;
    }
    int windowWidth = 0;
    int windowHeight = 0;
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);
    return PlankPresentation::mapWindowPointToDesktop(
                QPointF(windowX, windowY), QSize(windowWidth, windowHeight),
                QSize(drawableWidth, drawableHeight), *output, streamDimensions(),
                desktopPoint, allowClampedPosition);
}

bool SdlInputHandler::sendAbsoluteMousePosition(
        SDL_Window* window, float windowX, float windowY,
        bool allowClampedPosition)
{
    if (m_PresentationLayout.usesSourceRects()) {
        // One window per workstation display: positions go out in desktop
        // coordinates with the desktop as the reference, so they stay right
        // however the host packs its capture.
        QPointF desktopPoint;
        if (!mapWindowPointToDesktop(window, windowX, windowY, desktopPoint,
                                     allowClampedPosition)) {
            return false;
        }
        const QSize desktop = m_PresentationLayout.desktopSize;
        const QPoint position =
                PlankPresentation::absoluteDesktopPosition(desktopPoint, desktop);
        return LiSendMousePositionEvent(
                    static_cast<short>(position.x()),
                    static_cast<short>(position.y()),
                    static_cast<short>(desktop.width()),
                    static_cast<short>(desktop.height())) == 0;
    }

    PlankOutputGeometry geometry;
    if (!outputGeometry(window, geometry)) {
        return false;
    }

    const QSize streamSize = streamDimensions();
    QPointF streamPoint;
    if (!PlankPresentation::mapWindowPointToStream(
                QPointF(windowX, windowY), geometry, streamSize,
                streamPoint, allowClampedPosition)) {
        return false;
    }
    // Keep the coordinates inside the streamed image: the host treats
    // width/height as one past its last pixel.
    const QPoint position =
            PlankPresentation::absoluteStreamPosition(streamPoint, streamSize);
    return LiSendMousePositionEvent(
                static_cast<short>(position.x()),
                static_cast<short>(position.y()),
                static_cast<short>(streamSize.width()),
                static_cast<short>(streamSize.height())) == 0;
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
    activateCompositorCursor();

    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }

    if (!isMouseInVideoRegion(event->mouse_x, event->mouse_y,
                              event->windowID)) {
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

bool SdlInputHandler::isMouseInVideoRegion(float mouseX, float mouseY,
                                           Uint32 windowId)
{
    SDL_Window* window = presentationWindow(windowId);
    if (window != nullptr && m_PresentationLayout.usesSourceRects()) {
        QPointF desktopPoint;
        return mapWindowPointToDesktop(window, mouseX, mouseY, desktopPoint, false);
    }
    PlankOutputGeometry geometry;
    if (window == nullptr || !outputGeometry(window, geometry)) {
        return false;
    }

    QPointF streamPoint;
    return PlankPresentation::mapWindowPointToStream(
                QPointF(mouseX, mouseY), geometry, streamDimensions(),
                streamPoint, false);
}

bool SdlInputHandler::outputGeometry(SDL_Window* window,
                                     PlankOutputGeometry& geometry) const
{
    const auto* output = presentationOutput(window);
    if (output == nullptr) {
        return false;
    }

    int windowWidth = 0;
    int windowHeight = 0;
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    SDL_GetWindowSizeInPixels(window, &drawableWidth, &drawableHeight);
    geometry = PlankPresentation::outputGeometry(
                m_PresentationLayout, *output,
                QSize(windowWidth, windowHeight),
                QSize(drawableWidth, drawableHeight),
                m_LiveDrawableGeometry.load(std::memory_order_relaxed));
    return geometry.isValid();
}

SDL_Window* SdlInputHandler::presentationWindow(Uint32 windowId) const
{
    if (windowId == 0) {
        return m_Window;
    }
    SDL_Window* window = SDL_GetWindowFromID(windowId);
    return presentationOutput(window) != nullptr ? window : nullptr;
}

void SdlInputHandler::followPointerFocus(SDL_Window* target,
                                        SDL_MouseButtonFlags eventButtons,
                                        PointerFocusPosition position)
{
#ifdef Q_OS_DARWIN
    // The fullscreen surfaces are one remote desktop. Transfer native focus
    // on hover so a secondary click does not need a preceding activation click.
    // Never switch away from the Cocoa window that owns an in-progress drag.
    if (!m_PresentationLayout.isMultiOutput() || !isCaptureActive() ||
            eventButtons != 0 || SDL_GetMouseState(nullptr, nullptr) != 0) {
        return;
    }
    const bool tablet = position == PointerFocusPosition::HostTablet;
    if (tablet && !PlankPointerLogic::tabletFocusPositionIsCurrent(
                m_TabletCursorActive, m_AppliedRemoteCursorPositionValid,
                m_AppliedRemoteCursorPositionSequence, m_TabletCursorActivationSequence)) {
        return;
    }
    SDL_Window* focused = SDL_GetKeyboardFocus();
    if (focused == nullptr || focused == target ||
            presentationOutput(focused) == nullptr ||
            presentationOutput(target) == nullptr) {
        return;
    }
    const auto targetFlags = SDL_GetWindowFlags(target);
    if (!(targetFlags & SDL_WINDOW_FULLSCREEN) ||
            (targetFlags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) {
        return;
    }

    // Raw tablet reports go directly to the Host and do not move the Mac's
    // mouse pointer. Their target was mapped from the fresh Host position.
    // Only local mouse events can be checked against the Mac desktop pointer.
    // Both paths retain native mouse drags and require owned presentation focus.
    float gx, gy;
    if (SDL_GetGlobalMouseState(&gx, &gy) != 0) {
        return;
    }
    if (!tablet) {
        int wx, wy, width, height;
        if (!SDL_GetWindowPosition(target, &wx, &wy) ||
                !SDL_GetWindowSize(target, &width, &height) ||
                gx < wx || gy < wy || gx >= wx + width || gy >= wy + height) {
            return;
        }
    }
    if (!SDL_RaiseWindow(target)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Unable to transfer fullscreen pointer focus: %s",
                    SDL_GetError());
    }
    else {
        SDL_LogInfo(tablet ? SDL_LOG_CATEGORY_APPLICATION : SDL_LOG_CATEGORY_INPUT,
                    "PLANK fullscreen %s focus: %u -> %u",
                    tablet ? "tablet" : "pointer",
                    SDL_GetWindowID(focused), SDL_GetWindowID(target));
    }
#else
    Q_UNUSED(target);
    Q_UNUSED(eventButtons);
    Q_UNUSED(position);
#endif
}

const PlankPresentationOutput* SdlInputHandler::presentationOutput(
        SDL_Window* window) const
{
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window == window) {
            return &output;
        }
    }
    return nullptr;
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
    PlankOutputGeometry geometry;
    if (isCaptureActive() && m_PointerRegionLockActive &&
            outputGeometry(m_Window, geometry)) {
        // Use the same video rectangle the pointer is mapped into.
        const QRect video = PlankPresentation::videoRectInWindow(
                    streamDimensions(), geometry).toAlignedRect()
                .intersected(QRect(QPoint(0, 0), geometry.windowSize));
        const PlankPointerLogic::Rect windowRect = {
            0, 0, geometry.windowSize.width(), geometry.windowSize.height()
        };

        // A PLANK toolbar is anchored to the window's top edge, not
        // the scaled video's top edge. Keep the pointer inside the window while
        // allowing it to cross letterbox/pillarbox regions and reach the reveal
        // strip. Mouse motion outside the video rectangle remains local and is
        // not forwarded to the host by handleMouseMotionEvent().
        const auto confinementRect =
                PlankPointerLogic::pointerConfinementRect(
                    windowRect,
                    {video.x(), video.y(), video.width(), video.height()},
                    m_LocalToolbarAvailable);
        const SDL_Rect dst = {
            confinementRect.x,
            confinementRect.y,
            confinementRect.w,
            confinementRect.h,
        };

        SDL_SetWindowMouseRect(m_Window, &dst);
    }
    else {
        // Allow the cursor to leave the bounds of our video region or window
        SDL_SetWindowMouseRect(m_Window, nullptr);
    }
}
