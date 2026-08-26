#include "stationconnecttoolbar.h"

#include "input/input.h"
#include "settings/streamingpreferences.h"

#include <Limelight.h>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {
constexpr int ToolbarPreferredWidth = 539;
constexpr int ToolbarHeight = 39;
constexpr int EdgeRevealHeight = 3;
constexpr Uint32 EdgeActivationDelayMs = 1000;
constexpr Uint32 AutoHideDelayMs = 5000;
constexpr Uint32 BitrateSettleDelayMs = 250;
constexpr Uint32 BitrateAcknowledgementRetryMs = 1000;
constexpr Uint32 RedrawIntervalMs = 200;
constexpr Uint32 ToolbarMoveRedrawIntervalMs = 16;
constexpr int BitrateMinimumKbps = 10000;
constexpr int BitrateMaximumKbps = 150000;
constexpr int BitrateStepKbps = 500;
constexpr qreal WindowGlyphScale = 0.85;
constexpr qreal WindowButtonSize = 28.0 * WindowGlyphScale;
constexpr qreal WindowButtonRadius = 5.0 * WindowGlyphScale;

QColor interpolateColor(const QColor& start, const QColor& end, qreal fraction)
{
    fraction = qBound(0.0, fraction, 1.0);
    return QColor(qRound(start.red() + (end.red() - start.red()) * fraction),
                  qRound(start.green() + (end.green() - start.green()) * fraction),
                  qRound(start.blue() + (end.blue() - start.blue()) * fraction));
}

QColor packetLossColor(float packetLossPercent)
{
    const QColor blue(52, 132, 228);
    const QColor green(52, 199, 110);
    const QColor red(239, 88, 88);
    const qreal clampedLoss = qBound(0.0, qreal(packetLossPercent), 10.0);

    if (clampedLoss <= 5.0) {
        return interpolateColor(blue, green, clampedLoss / 5.0);
    }
    return interpolateColor(green, red, (clampedLoss - 5.0) / 5.0);
}
}

StationConnectToolbar::StationConnectToolbar(
        SDL_Window* window,
        SdlInputHandler& inputHandler,
        StreamingPreferences& preferences)
    : m_Window(window),
      m_ToolbarWindow(nullptr),
      m_ToolbarWindowId(0),
      m_InputHandler(inputHandler),
      m_Preferences(preferences),
      m_Visible(false),
      m_Pinned(preferences.stationConnectToolbarPinned),
      m_DraggingToolbar(false),
      m_DraggingSlider(false),
      m_PointerInside(false),
      m_PointerInitialized(false),
      m_LocalPointerInteraction(false),
      m_BitrateSupported(
              (LiGetHostFeatureFlags() &
               (LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK)) ==
              (LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK)),
      m_WindowWidth(0),
      m_WindowHeight(0),
      m_WindowPixelWidth(0),
      m_WindowPixelHeight(0),
      m_PixelDensity(1.0f),
      m_Width(ToolbarPreferredWidth),
      m_ToolbarLeft(-1),
      m_PointerX(0),
      m_PointerY(0),
      m_PressedControl(Control::None),
      m_BitrateKbps(qBound(BitrateMinimumKbps,
                           preferences.bitrateKbps,
                           BitrateMaximumKbps)),
      m_LastSentBitrateKbps(-1),
      m_AppliedBitrateKbps(-1),
      m_AppliedPeakKbps(-1),
      m_RenderedFps(0.0f),
      m_VideoMbps(0.0f),
      m_PacketLossPercent(-1.0f),
      m_LastDrawnFps(-1.0f),
      m_LastDrawnVideoMbps(-1.0f),
      m_LastDrawnPacketLossPercent(-2.0f),
      m_HideDeadline(0),
      m_LastBitrateSendTime(0),
      m_LastBitrateChangeTime(0),
      m_LastToolbarMoveDrawTime(0),
      m_LastRedrawTime(0),
      m_EdgeHoverStartTime(0)
{
    if (m_Preferences.bitrateKbps != m_BitrateKbps) {
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
    }
    notifyWindowChanged();
    if (createToolbarWindow()) {
        m_InputHandler.setLocalToolbarAvailable(true);
        if (m_Pinned) {
            show(SDL_GetTicks());
        }
    }
    // The host receives the exact target in RTSP. Send it over the live
    // control stream too, then retry until the host confirms its applied
    // target.
    queueBitrateRequest(SDL_GetTicks(), true);
}

StationConnectToolbar::~StationConnectToolbar()
{
    endLocalPointerInteraction();
    m_InputHandler.setLocalToolbarAvailable(false);
    if (m_ToolbarWindow != nullptr) {
        SDL_DestroyWindow(m_ToolbarWindow);
        m_ToolbarWindow = nullptr;
        m_ToolbarWindowId = 0;
    }
}

void StationConnectToolbar::setRenderedStats(
        float fps, float videoMbps, float packetLossPercent)
{
    m_RenderedFps = std::max(0.0f, fps);
    m_VideoMbps = std::max(0.0f, videoMbps);
    m_PacketLossPercent = packetLossPercent < 0.0f ?
                -1.0f : qBound(0.0f, packetLossPercent, 100.0f);
}

void StationConnectToolbar::setAppliedBitrate(
        int requestedKbps, int appliedKbps, int peakKbps)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "StationConnect encoder target confirmed: requested=%d Kbps, applied=%d Kbps, peak=%d Kbps",
                requestedKbps, appliedKbps, peakKbps);

    // An acknowledgement for an older slider position must not suppress the
    // newest pending target. The normal update loop will continue retrying.
    if (requestedKbps != m_BitrateKbps) {
        return;
    }

    m_AppliedBitrateKbps = appliedKbps;
    m_AppliedPeakKbps = peakKbps;
    if (appliedKbps != m_BitrateKbps) {
        m_BitrateKbps = appliedKbps;
        m_Preferences.bitrateKbps = appliedKbps;
        m_Preferences.save();
    }
    redraw();
}

void StationConnectToolbar::update(Uint64 now)
{
    if (!m_Visible && !m_LocalPointerInteraction &&
            m_EdgeHoverStartTime != 0 &&
            m_PointerY <= EdgeRevealHeight &&
            now >= m_EdgeHoverStartTime + EdgeActivationDelayMs) {
        show(now);
    }

    if (m_Visible && !m_ButtonRouter.hasLocalButtons() &&
            !m_DraggingSlider && !m_PointerInside &&
            m_HideDeadline != 0 && now >= m_HideDeadline) {
        if (m_Pinned) {
            endLocalPointerInteraction();
            m_HideDeadline = 0;
        } else {
            hide();
        }
    }

    if (m_Visible && now >= m_LastRedrawTime + RedrawIntervalMs &&
            (std::fabs(m_RenderedFps - m_LastDrawnFps) >= 0.05f ||
             std::fabs(m_VideoMbps - m_LastDrawnVideoMbps) >= 0.05f ||
             std::fabs(m_PacketLossPercent -
                       m_LastDrawnPacketLossPercent) >= 0.05f)) {
        redraw();
    }

    queueBitrateRequest(now, false);
}

void StationConnectToolbar::notifyWindowChanged()
{
    const int oldLogicalWidth = m_WindowWidth;
    const int oldLogicalHeight = m_WindowHeight;
    const int oldPixelWidth = m_WindowPixelWidth;
    const int oldPixelHeight = m_WindowPixelHeight;
    const float oldPixelDensity = m_PixelDensity;
    const float oldHorizontalPosition = m_ToolbarLeft >= 0 ?
                StationConnectToolbarLogic::horizontalPosition(
                    m_ToolbarLeft, m_WindowWidth, m_Width) : 0.5f;

    SDL_GetWindowSize(m_Window, &m_WindowWidth, &m_WindowHeight);
    SDL_GetWindowSizeInPixels(m_Window, &m_WindowPixelWidth,
                              &m_WindowPixelHeight);
    m_Width = std::min(ToolbarPreferredWidth, std::max(m_WindowWidth, 1));
    if (m_ToolbarLeft < 0) {
        m_ToolbarLeft = std::max(0, (m_WindowWidth - m_Width) / 2);
    } else {
        m_ToolbarLeft = StationConnectToolbarLogic::logicalLeftFromPosition(
                    oldHorizontalPosition, m_WindowWidth, m_Width);
    }
    if (!m_PointerInitialized) {
        m_PointerX = m_WindowWidth / 2;
        m_PointerY = m_WindowHeight / 2;
        m_PointerInitialized = true;
    } else {
        m_PointerX = qBound(0, m_PointerX, std::max(0, m_WindowWidth - 1));
        m_PointerY = qBound(0, m_PointerY, std::max(0, m_WindowHeight - 1));
    }
    if (m_ToolbarWindow != nullptr) {
        if (!SDL_SetWindowSize(m_ToolbarWindow, m_Width, ToolbarHeight)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Failed to resize StationConnect toolbar window: %s",
                        SDL_GetError());
        }
        positionToolbarWindow();
        updateToolbarWindowMetrics();
        if (m_Visible) {
            redraw();
        }
    }
    else {
        m_PixelDensity = StationConnectToolbarLogic::resolvePixelDensity(
                    SDL_GetWindowPixelDensity(m_Window),
                    m_WindowWidth, m_WindowHeight,
                    m_WindowPixelWidth, m_WindowPixelHeight);
    }

    if (oldLogicalWidth != m_WindowWidth ||
            oldLogicalHeight != m_WindowHeight ||
            oldPixelWidth != m_WindowPixelWidth ||
            oldPixelHeight != m_WindowPixelHeight ||
            std::fabs(oldPixelDensity - m_PixelDensity) >= 0.001f) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "StationConnect toolbar geometry: logical=%dx%d, pixels=%dx%d, density=%.3f, surface=%dx%d",
                    m_WindowWidth, m_WindowHeight,
                    m_WindowPixelWidth, m_WindowPixelHeight,
                    m_PixelDensity,
                    StationConnectToolbarLogic::physicalExtent(
                        m_Width, m_PixelDensity),
                    StationConnectToolbarLogic::physicalExtent(
                        ToolbarHeight, m_PixelDensity));
    }
}

bool StationConnectToolbar::createToolbarWindow()
{
    const SDL_WindowFlags flags = SDL_WINDOW_POPUP_MENU |
            SDL_WINDOW_NOT_FOCUSABLE |
            SDL_WINDOW_HIGH_PIXEL_DENSITY |
            SDL_WINDOW_HIDDEN;
    m_ToolbarWindow = SDL_CreatePopupWindow(
                m_Window, toolbarLeft(), 0, m_Width, ToolbarHeight, flags);
    if (m_ToolbarWindow == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create native StationConnect toolbar window: %s",
                     SDL_GetError());
        return false;
    }

    m_ToolbarWindowId = SDL_GetWindowID(m_ToolbarWindow);
    SDL_SetWindowTitle(m_ToolbarWindow, "StationConnect Toolbar");
    if (!SDL_SetWindowFocusable(m_ToolbarWindow, false)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to make StationConnect toolbar non-focusable: %s",
                    SDL_GetError());
    }
    positionToolbarWindow();
    updateToolbarWindowMetrics();
    redraw();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Created native StationConnect toolbar window: id=%u logical=%dx%d density=%.3f",
                m_ToolbarWindowId, m_Width, ToolbarHeight, m_PixelDensity);
    return true;
}

void StationConnectToolbar::positionToolbarWindow()
{
    if (m_ToolbarWindow == nullptr) {
        return;
    }
    if (!SDL_SetWindowPosition(m_ToolbarWindow, toolbarLeft(), 0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to position StationConnect toolbar window: %s",
                    SDL_GetError());
    }
}

void StationConnectToolbar::updateToolbarWindowMetrics()
{
    if (m_ToolbarWindow == nullptr) {
        return;
    }

    int logicalWidth = m_Width;
    int logicalHeight = ToolbarHeight;
    int pixelWidth = StationConnectToolbarLogic::physicalExtent(
                logicalWidth, m_PixelDensity);
    int pixelHeight = StationConnectToolbarLogic::physicalExtent(
                logicalHeight, m_PixelDensity);
    SDL_GetWindowSize(m_ToolbarWindow, &logicalWidth, &logicalHeight);
    SDL_GetWindowSizeInPixels(m_ToolbarWindow, &pixelWidth, &pixelHeight);
    m_PixelDensity = StationConnectToolbarLogic::resolvePixelDensity(
                SDL_GetWindowPixelDensity(m_ToolbarWindow),
                logicalWidth, logicalHeight, pixelWidth, pixelHeight);
}

void StationConnectToolbar::notifyFocusLost()
{
    m_ButtonRouter.reset();
    m_PressedControl = Control::None;
    m_DraggingToolbar = false;
    m_DraggingSlider = false;
    endLocalPointerInteraction();
}

bool StationConnectToolbar::handleWindowEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        break;
    default:
        return false;
    }

    if (event.window.windowID != m_ToolbarWindowId ||
            m_ToolbarWindowId == 0) {
        return false;
    }

    const Uint64 now = SDL_GetTicks();
    switch (event.type) {
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        m_PointerInside = true;
        m_HideDeadline = 0;
        beginLocalPointerInteraction();
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        m_PointerInside = false;
        if (!m_ButtonRouter.hasLocalButtons()) {
            endLocalPointerInteraction();
            m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        m_ButtonRouter.reset();
        m_PressedControl = Control::None;
        m_DraggingToolbar = false;
        m_DraggingSlider = false;
        endLocalPointerInteraction();
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        updateToolbarWindowMetrics();
        redraw();
        break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
        redraw();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        hide();
        break;
    default:
        break;
    }
    return true;
}

bool StationConnectToolbar::handleMouseMotion(SDL_MouseMotionEvent& event)
{
    const bool fromToolbar = event.windowID == m_ToolbarWindowId &&
            m_ToolbarWindowId != 0;
    if (event.which == SDL_TOUCH_MOUSEID) {
        return fromToolbar;
    }

    const Uint64 now = SDL_GetTicks();
    if (fromToolbar) {
        m_PointerX = StationConnectToolbarLogic::parentXFromPopup(
                    qRound(event.x), toolbarLeft());
        m_PointerY = qRound(event.y);
        m_PointerInside = true;
        m_HideDeadline = 0;
    }
    else if (m_ButtonRouter.hasLocalButtons()) {
        // A local drag may cross the popup boundary. Continue it from relative
        // motion so it never depends on the parent window's absolute viewport.
        m_PointerX = qBound(0, m_PointerX + qRound(event.xrel),
                            std::max(0, m_WindowWidth - 1));
        m_PointerY = qBound(0, m_PointerY + qRound(event.yrel),
                            std::max(0, m_WindowHeight - 1));
        m_PointerInside = false;
    }
    else {
        m_PointerX = qRound(event.x);
        m_PointerY = qRound(event.y);

        // The parent video window is responsible only for the one-second
        // top-edge dwell. Once shown, the native child window owns its own hit
        // testing and pointer events.
        if (!m_Visible && !m_LocalPointerInteraction) {
            if (m_PointerY <= EdgeRevealHeight) {
                if (m_EdgeHoverStartTime == 0) {
                    m_EdgeHoverStartTime = now;
                } else if (now >= m_EdgeHoverStartTime +
                           EdgeActivationDelayMs) {
                    show(now);
                }
            } else {
                m_EdgeHoverStartTime = 0;
            }
        }
    }

    const auto owner = m_ButtonRouter.routeMotion(fromToolbar);
    if (owner == StationConnectToolbarLogic::ButtonRouter::Owner::Remote) {
        if (fromToolbar) {
            // Preserve a remote press/release sequence that crosses the child
            // window by translating its motion back into parent coordinates.
            event.x = static_cast<float>(m_PointerX);
            event.y = static_cast<float>(m_PointerY);
        }
        return false;
    }

    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }

    if (m_DraggingToolbar) {
        const int newLeft = StationConnectToolbarLogic::movedPopupLeft(
                    m_ToolbarLeft, event.xrel, m_WindowWidth, m_Width);
        if (newLeft != m_ToolbarLeft) {
            m_ToolbarLeft = newLeft;
            positionToolbarWindow();
            if (now >= m_LastToolbarMoveDrawTime + ToolbarMoveRedrawIntervalMs) {
                redraw();
                m_LastToolbarMoveDrawTime = now;
            }
        }
        return true;
    }

    if (m_DraggingSlider) {
        updateBitrateFromPointer(m_PointerX, now, false);
        redraw();
        return true;
    }

    redraw();
    return true;
}

StationConnectToolbar::Action StationConnectToolbar::handleMouseButton(
        SDL_MouseButtonEvent& event)
{
    const bool fromToolbar = event.windowID == m_ToolbarWindowId &&
            m_ToolbarWindowId != 0;
    if (fromToolbar) {
        event.x += static_cast<float>(toolbarLeft());
    }
    m_PointerX = qRound(event.x);
    m_PointerY = qRound(event.y);
    const int pointerX = m_PointerX;
    const int pointerY = m_PointerY;
    const Uint64 now = SDL_GetTicks();
    const bool pointerInside = m_Visible && fromToolbar;
    const auto owner = m_ButtonRouter.routeButton(
                event.button, event.down, pointerInside);
    if (owner == StationConnectToolbarLogic::ButtonRouter::Owner::Remote) {
        if (!m_ButtonRouter.hasLocalButtons()) {
            endLocalPointerInteraction();
        }
        return Action::None;
    }

    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }

    m_PointerInside = pointerInside;
    m_HideDeadline = 0;

    if (event.button != SDL_BUTTON_LEFT) {
        return Action::Consumed;
    }

    if (event.down) {
        m_PressedControl = fromToolbar ?
                    controlAt(pointerX, pointerY) : Control::None;
        if (m_PressedControl == Control::Handle) {
            m_DraggingToolbar = true;
            redraw();
        } else if (m_PressedControl == Control::Slider &&
                   m_BitrateSupported) {
            m_DraggingSlider = true;
            updateBitrateFromPointer(pointerX, now, false);
        }
        return Action::Consumed;
    }

    const Control releasedControl = fromToolbar ?
                controlAt(pointerX, pointerY) : Control::None;
    const Control pressedControl = m_PressedControl;
    m_PressedControl = Control::None;

    if (m_DraggingToolbar) {
        m_DraggingToolbar = false;
        redraw();
    }
    if (m_DraggingSlider) {
        updateBitrateFromPointer(pointerX, now, true);
        m_DraggingSlider = false;
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
    }

    Action action = Action::Consumed;
    if (pressedControl == releasedControl) {
        switch (pressedControl) {
        case Control::Pin:
            m_Pinned = !m_Pinned;
            m_Preferences.stationConnectToolbarPinned = m_Pinned;
            m_Preferences.save();
            redraw();
            break;
        case Control::Fullscreen:
            action = Action::ToggleFullscreen;
            break;
        case Control::Minimize:
            action = Action::Minimize;
            break;
        case Control::Disconnect:
            action = Action::Disconnect;
            break;
        default:
            break;
        }
    }

    if (!m_ButtonRouter.hasLocalButtons() && !fromToolbar) {
        m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
        endLocalPointerInteraction();
    }
    return action;
}

bool StationConnectToolbar::handleMouseWheel(SDL_MouseWheelEvent& event)
{
    const bool fromToolbar = event.windowID == m_ToolbarWindowId &&
            m_ToolbarWindowId != 0;
    if (!fromToolbar) {
        return false;
    }

    const int mouseX = StationConnectToolbarLogic::parentXFromPopup(
                qRound(event.mouse_x), toolbarLeft());
    const int mouseY = qRound(event.mouse_y);
    m_PointerX = mouseX;
    m_PointerY = mouseY;
    if (!m_Visible) {
        return false;
    }
    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }
    if (!m_BitrateSupported || !sliderContains(mouseX, mouseY)) {
        return true;
    }

    const int delta = event.y > 0 ? BitrateStepKbps :
                      event.y < 0 ? -BitrateStepKbps : 0;
    if (delta != 0) {
        m_BitrateKbps = qBound(BitrateMinimumKbps,
                               m_BitrateKbps + delta, BitrateMaximumKbps);
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
        redraw();
        queueBitrateRequest(SDL_GetTicks(), true);
    }
    return true;
}

int StationConnectToolbar::eventWaitTimeout() const
{
    return (m_Visible || m_EdgeHoverStartTime != 0) ? 50 : 1000;
}

void StationConnectToolbar::show(Uint64 now)
{
    if (m_ToolbarWindow == nullptr) {
        return;
    }

    positionToolbarWindow();
    redraw();
    if (!SDL_ShowWindow(m_ToolbarWindow)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to show StationConnect toolbar window: %s",
                    SDL_GetError());
        return;
    }

    m_Visible = true;
    m_EdgeHoverStartTime = 0;
    m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
}

void StationConnectToolbar::hide()
{
    if (m_ToolbarWindow != nullptr &&
            !SDL_HideWindow(m_ToolbarWindow)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to hide StationConnect toolbar window: %s",
                    SDL_GetError());
    }
    endLocalPointerInteraction();
    m_Visible = false;
    m_EdgeHoverStartTime = 0;
    m_PointerInside = false;
    m_HideDeadline = 0;
}

void StationConnectToolbar::beginLocalPointerInteraction()
{
    if (m_LocalPointerInteraction) {
        return;
    }

    // This is the equivalent of RGS routing an event to its toolbar widget:
    // keep the stream's input mode untouched and expose the receiver cursor
    // while the toolbar owns the pointer.
    m_InputHandler.setToolbarInteractionActive(true);

    m_LocalPointerInteraction = true;
    m_PointerInside = true;
    m_HideDeadline = 0;
    redraw();
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "StationConnect toolbar routed pointer to local controls");
}

void StationConnectToolbar::endLocalPointerInteraction()
{
    if (!m_LocalPointerInteraction) {
        return;
    }

    m_LocalPointerInteraction = false;
    m_DraggingToolbar = false;
    m_DraggingSlider = false;
    m_PointerInside = false;

    // The next absolute event resumes host routing at the same coordinate; no
    // warp, flush, capture toggle, or synthetic synchronization event.
    m_InputHandler.setToolbarInteractionActive(false);
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "StationConnect toolbar routed pointer to remote desktop");
}

void StationConnectToolbar::redraw()
{
    if (m_ToolbarWindow == nullptr) {
        return;
    }

    updateToolbarWindowMetrics();
    const int surfaceWidth =
            StationConnectToolbarLogic::physicalExtent(m_Width, m_PixelDensity);
    const int surfaceHeight =
            StationConnectToolbarLogic::physicalExtent(ToolbarHeight,
                                                       m_PixelDensity);
    SDL_Surface* surface = SDL_CreateSurface(
                surfaceWidth, surfaceHeight, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to allocate StationConnect toolbar surface: %s",
                    SDL_GetError());
        return;
    }

    QImage image(static_cast<uchar*>(surface->pixels), surface->w, surface->h,
                 surface->pitch, QImage::Format_ARGB32_Premultiplied);
    // The remote host cursor is already composited into the video frame. The
    // local Wayland cursor becomes visible when this popup owns the pointer,
    // so the toolbar must be opaque or the frozen remote cursor shows through
    // as a distracting second pointer. Painting every row also ensures that
    // the native window never presents uninitialized pixels below the UI.
    image.fill(QColor(22, 27, 34));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(m_PixelDensity, m_PixelDensity);
    // Paint the background through the first pixel row so the toolbar meets
    // the physical top edge without an antialiased one-pixel gap.
    painter.fillRect(QRect(0, 0, m_Width, ToolbarHeight),
                     QColor(22, 27, 34));
    painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, m_Width - 1.0,
                                   ToolbarHeight - 1.0), 0, 0);

    QFont labelFont;
    labelFont.setPixelSize(10);
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);
    painter.setPen(QColor(235, 239, 244));

    // Six-dot grip for moving the toolbar horizontally along the top edge.
    const bool handleHovered = m_LocalPointerInteraction &&
                               handleContains(m_PointerX, m_PointerY);
    painter.setPen(Qt::NoPen);
    painter.setBrush(handleHovered || m_DraggingToolbar ?
                         QColor(205, 214, 224) : QColor(123, 134, 147));
    for (int y : {15, 19, 24}) {
        painter.drawEllipse(QPointF(9, y), 1.2, 1.2);
        painter.drawEllipse(QPointF(14, y), 1.2, 1.2);
    }

    // RGS-style upright outline thumbtack. A slash indicates auto-hide mode;
    // the unmodified thumbtack indicates that the toolbar is pinned.
    const bool pinHovered = m_LocalPointerInteraction &&
                            pinContains(m_PointerX, m_PointerY);
    QPainterPath pin;
    pin.moveTo(31, 11);
    pin.lineTo(43, 11);
    pin.lineTo(40, 15);
    pin.lineTo(40, 19);
    pin.lineTo(43, 23);
    pin.lineTo(38, 23);
    pin.lineTo(37, 29);
    pin.lineTo(36, 23);
    pin.lineTo(30, 23);
    pin.lineTo(33, 19);
    pin.lineTo(33, 15);
    pin.closeSubpath();
    painter.setBrush(Qt::NoBrush);
    const QColor pinColor = pinHovered ? QColor(255, 255, 255) :
                                         QColor(238, 242, 247);
    painter.setPen(QPen(pinColor, 1.2, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(pin);
    if (!m_Pinned) {
        painter.setPen(QPen(pinColor, 1.6, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(28, 8), QPointF(46, 30));
    }

    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(57, 5, 34, 12), Qt::AlignLeft | Qt::AlignVCenter, "FPS");
    QFont valueFont = labelFont;
    valueFont.setPixelSize(13);
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(57, 16, 50, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_RenderedFps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(104, 5, 65, 12), Qt::AlignLeft | Qt::AlignVCenter,
                     "Video Mbps");
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(104, 16, 65, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_VideoMbps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(172, 5, 52, 12), Qt::AlignLeft | Qt::AlignVCenter,
                     "Loss");
    const QColor lossColor = m_PacketLossPercent < 0.0f ?
                QColor(112, 120, 130) : packetLossColor(m_PacketLossPercent);
    QFont lossFont = valueFont;
    lossFont.setPixelSize(12);
    painter.setFont(lossFont);
    painter.setPen(lossColor);
    painter.drawText(QRect(174, 16, 52, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     m_PacketLossPercent < 0.0f ?
                         QString("--") :
                         QString("%1%").arg(m_PacketLossPercent, 0, 'f', 1));

    QFont targetFont = labelFont;
    targetFont.setPixelSize(12);
    painter.setFont(targetFont);
    painter.setPen(m_BitrateSupported ? QColor(235, 239, 244) : QColor(135, 143, 153));
    painter.drawText(QRect(229, 3, 190, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString("Encoder target  %1 Mbps").arg(m_BitrateKbps / 1000.0, 0, 'f', 1));

    const int trackLeft = sliderLeft() - toolbarLeft();
    const int trackRight = sliderRight() - toolbarLeft();
    const int trackY = 27;
    painter.setPen(QPen(QColor(92, 102, 114), 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(trackLeft, trackY, trackRight, trackY);

    const qreal fraction = qreal(m_BitrateKbps - BitrateMinimumKbps) /
                           qreal(BitrateMaximumKbps - BitrateMinimumKbps);
    const int thumbX = trackLeft + qRound(fraction * (trackRight - trackLeft));
    if (m_BitrateSupported) {
        painter.setPen(QPen(QColor(52, 132, 228), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(trackLeft, trackY, thumbX, trackY);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_BitrateSupported ? QColor(243, 246, 250) : QColor(112, 120, 130));
    painter.drawEllipse(QPointF(thumbX, trackY), 5, 5);

    const QPointF fullscreenCenter(m_Width - 88.0, 19.0);
    const QRectF fullscreenRect(fullscreenCenter.x() - WindowButtonSize / 2.0,
                                fullscreenCenter.y() - WindowButtonSize / 2.0,
                                WindowButtonSize,
                                WindowButtonSize);
    const bool fullscreenHovered = m_LocalPointerInteraction &&
                                   fullscreenContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(104, 116, 131), 1));
    painter.setBrush(fullscreenHovered ? QColor(68, 78, 90, 230) :
                                        QColor(48, 57, 68, 210));
    painter.drawRoundedRect(fullscreenRect,
                            WindowButtonRadius,
                            WindowButtonRadius);

    // Four corners point outward when entering fullscreen and inward when the
    // next click will restore the decorated window.
    const bool isFullscreen =
            (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_FULLSCREEN) != 0;
    const qreal outer = 6.0 * WindowGlyphScale;
    const qreal inner = 2.0 * WindowGlyphScale;
    const qreal cornerDistance = isFullscreen ? inner : outer;
    const qreal armLength = outer - inner;
    painter.setPen(QPen(QColor(226, 232, 239), 1.4,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const qreal sx : {-1.0, 1.0}) {
        for (const qreal sy : {-1.0, 1.0}) {
            const QPointF corner = fullscreenCenter +
                    QPointF(sx * cornerDistance, sy * cornerDistance);
            painter.drawLine(corner,
                             corner + QPointF((isFullscreen ? sx : -sx) *
                                              armLength, 0));
            painter.drawLine(corner,
                             corner + QPointF(0, (isFullscreen ? sy : -sy) *
                                              armLength));
        }
    }

    const QPointF minimizeCenter(m_Width - 55.0, 19.0);
    const QRectF minimizeRect(minimizeCenter.x() - WindowButtonSize / 2.0,
                              minimizeCenter.y() - WindowButtonSize / 2.0,
                              WindowButtonSize,
                              WindowButtonSize);
    const bool minimizeHovered = m_LocalPointerInteraction &&
                                 minimizeContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(104, 116, 131), 1));
    painter.setBrush(minimizeHovered ? QColor(68, 78, 90, 230) :
                                      QColor(48, 57, 68, 210));
    painter.drawRoundedRect(minimizeRect,
                            WindowButtonRadius,
                            WindowButtonRadius);
    painter.setPen(QPen(QColor(226, 232, 239), 1.5, Qt::SolidLine, Qt::RoundCap));
    const qreal minimizeHalfWidth = 5.0 * WindowGlyphScale;
    painter.drawLine(QPointF(minimizeCenter.x() - minimizeHalfWidth, 21),
                     QPointF(minimizeCenter.x() + minimizeHalfWidth, 21));

    const QPointF disconnectCenter(m_Width - 22.0, 19.0);
    const QRectF disconnectRect(disconnectCenter.x() - WindowButtonSize / 2.0,
                                disconnectCenter.y() - WindowButtonSize / 2.0,
                                WindowButtonSize,
                                WindowButtonSize);
    const bool disconnectHovered = m_LocalPointerInteraction &&
                                   disconnectContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(239, 88, 88), 1));
    painter.setBrush(disconnectHovered ? QColor(151, 43, 49, 220) :
                                        QColor(126, 35, 40, 185));
    painter.drawRoundedRect(disconnectRect,
                            WindowButtonRadius,
                            WindowButtonRadius);
    painter.setPen(QPen(QColor(255, 228, 228), 1.5, Qt::SolidLine, Qt::RoundCap));
    const qreal disconnectHalfExtent = 6.0 * WindowGlyphScale;
    painter.drawLine(
                disconnectCenter + QPointF(-disconnectHalfExtent,
                                            -disconnectHalfExtent),
                disconnectCenter + QPointF(disconnectHalfExtent,
                                            disconnectHalfExtent));
    painter.drawLine(
                disconnectCenter + QPointF(disconnectHalfExtent,
                                            -disconnectHalfExtent),
                disconnectCenter + QPointF(-disconnectHalfExtent,
                                            disconnectHalfExtent));

    if (!m_BitrateSupported) {
        QFont hintFont = labelFont;
        hintFont.setPixelSize(7);
        painter.setFont(hintFont);
        painter.setPen(QColor(183, 151, 92));
        painter.drawText(QRect(229, 28, 187, 9), Qt::AlignLeft | Qt::AlignVCenter,
                         "Host update required for live control");
    }

    painter.end();
    m_LastDrawnFps = m_RenderedFps;
    m_LastDrawnVideoMbps = m_VideoMbps;
    m_LastDrawnPacketLossPercent = m_PacketLossPercent;
    m_LastRedrawTime = SDL_GetTicks();

    SDL_Surface* windowSurface = SDL_GetWindowSurface(m_ToolbarWindow);
    if (windowSurface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to acquire StationConnect toolbar window surface: %s",
                    SDL_GetError());
        SDL_DestroySurface(surface);
        return;
    }
    if (!SDL_BlitSurfaceScaled(surface, nullptr, windowSurface, nullptr,
                               SDL_SCALEMODE_LINEAR)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to paint StationConnect toolbar window: %s",
                    SDL_GetError());
        SDL_DestroySurface(surface);
        return;
    }
    SDL_DestroySurface(surface);
    if (!SDL_UpdateWindowSurface(m_ToolbarWindow)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to present StationConnect toolbar window: %s",
                    SDL_GetError());
    }
}

void StationConnectToolbar::updateBitrateFromPointer(int x, Uint64 now, bool forceSend)
{
    const qreal fraction = qBound(
                qreal(0.0),
                qreal(x - sliderLeft()) / qreal(std::max(1, sliderRight() - sliderLeft())),
                qreal(1.0));
    int bitrate = BitrateMinimumKbps +
                  qRound(fraction * (BitrateMaximumKbps - BitrateMinimumKbps));
    bitrate = qRound(qreal(bitrate) / BitrateStepKbps) * BitrateStepKbps;
    bitrate = qBound(BitrateMinimumKbps, bitrate, BitrateMaximumKbps);
    if (bitrate != m_BitrateKbps) {
        m_BitrateKbps = bitrate;
        m_Preferences.bitrateKbps = bitrate;
        m_LastBitrateChangeTime = now;
        redraw();
    }
    queueBitrateRequest(now, forceSend);
}

void StationConnectToolbar::queueBitrateRequest(Uint64 now, bool forceSend)
{
    if (!m_BitrateSupported) {
        return;
    }
    if (m_BitrateKbps == m_AppliedBitrateKbps) {
        return;
    }
    if (!forceSend &&
            (now < m_LastBitrateChangeTime + BitrateSettleDelayMs ||
             now < m_LastBitrateSendTime +
                              (m_BitrateKbps == m_LastSentBitrateKbps ?
                                   BitrateAcknowledgementRetryMs :
                                   BitrateSettleDelayMs))) {
        return;
    }
    if (LiSetVideoBitrate(m_BitrateKbps) == 0) {
        m_LastSentBitrateKbps = m_BitrateKbps;
        m_LastBitrateSendTime = now;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Requested StationConnect encoder target: %d Kbps",
                    m_BitrateKbps);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to send StationConnect bitrate update: %d Kbps",
                    m_BitrateKbps);
    }
}

bool StationConnectToolbar::contains(int x, int y) const
{
    return x >= toolbarLeft() && x < toolbarLeft() + m_Width &&
           y >= 0 && y < ToolbarHeight;
}

bool StationConnectToolbar::sliderContains(int x, int y) const
{
    return x >= sliderLeft() - 7 && x <= sliderRight() + 7 &&
           y >= 17 && y <= 37;
}

bool StationConnectToolbar::pinContains(int x, int y) const
{
    return x >= toolbarLeft() + 23 && x <= toolbarLeft() + 51 &&
           y >= 5 && y <= 33;
}

bool StationConnectToolbar::handleContains(int x, int y) const
{
    return x >= toolbarLeft() && x <= toolbarLeft() + 22 &&
           y >= 0 && y < ToolbarHeight;
}

bool StationConnectToolbar::minimizeContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 69 &&
           x <= toolbarLeft() + m_Width - 41 && y >= 5 && y <= 33;
}

bool StationConnectToolbar::fullscreenContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 102 &&
           x <= toolbarLeft() + m_Width - 74 && y >= 5 && y <= 33;
}

bool StationConnectToolbar::disconnectContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 36 &&
           x <= toolbarLeft() + m_Width - 8 && y >= 5 && y <= 33;
}

StationConnectToolbar::Control StationConnectToolbar::controlAt(int x, int y) const
{
    if (!contains(x, y)) {
        return Control::None;
    }
    if (handleContains(x, y)) {
        return Control::Handle;
    }
    if (pinContains(x, y)) {
        return Control::Pin;
    }
    if (fullscreenContains(x, y)) {
        return Control::Fullscreen;
    }
    if (minimizeContains(x, y)) {
        return Control::Minimize;
    }
    if (disconnectContains(x, y)) {
        return Control::Disconnect;
    }
    if (sliderContains(x, y)) {
        return Control::Slider;
    }
    return Control::None;
}

int StationConnectToolbar::toolbarLeft() const
{
    return std::max(0, m_ToolbarLeft);
}

int StationConnectToolbar::sliderLeft() const
{
    return toolbarLeft() + 229;
}

int StationConnectToolbar::sliderRight() const
{
    return toolbarLeft() + std::max(191, m_Width - 113);
}
