#include "stationconnecttoolbar.h"

#include "input/input.h"
#include "settings/streamingpreferences.h"
#include "video/overlaymanager.h"

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
constexpr int ToolbarPreferredWidth = 506;
constexpr int ToolbarHeight = 43;
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
}

StationConnectToolbar::StationConnectToolbar(
        SDL_Window* window,
        Overlay::OverlayManager& overlayManager,
        SdlInputHandler& inputHandler,
        StreamingPreferences& preferences)
    : m_Window(window),
      m_OverlayManager(overlayManager),
      m_InputHandler(inputHandler),
      m_Preferences(preferences),
      m_Visible(preferences.stationConnectToolbarPinned),
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
      m_Width(ToolbarPreferredWidth),
      m_ToolbarLeft(-1),
      m_ToolbarDragOffsetX(0),
      m_PointerX(0),
      m_PointerY(0),
      m_BitrateKbps(qBound(BitrateMinimumKbps,
                           preferences.bitrateKbps,
                           BitrateMaximumKbps)),
      m_LastSentBitrateKbps(-1),
      m_AppliedBitrateKbps(-1),
      m_AppliedPeakKbps(-1),
      m_RenderedFps(0.0f),
      m_VideoMbps(0.0f),
      m_LastDrawnFps(-1.0f),
      m_LastDrawnVideoMbps(-1.0f),
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
    redraw();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, m_Visible);
    // The host receives the exact target in RTSP. Send it over the live
    // control stream too, then retry until the host confirms its applied
    // target.
    queueBitrateRequest(SDL_GetTicks(), true);
}

StationConnectToolbar::~StationConnectToolbar()
{
    endLocalPointerInteraction();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
}

void StationConnectToolbar::setRenderedStats(float fps, float videoMbps)
{
    m_RenderedFps = std::max(0.0f, fps);
    m_VideoMbps = std::max(0.0f, videoMbps);
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

void StationConnectToolbar::update(Uint32 now)
{
    if (!m_Visible && !m_LocalPointerInteraction &&
            m_EdgeHoverStartTime != 0 &&
            m_PointerY <= EdgeRevealHeight &&
            SDL_TICKS_PASSED(now,
                             m_EdgeHoverStartTime + EdgeActivationDelayMs)) {
        show(now);
    }

    if (m_Visible && !m_DraggingSlider && !m_PointerInside &&
            m_HideDeadline != 0 && SDL_TICKS_PASSED(now, m_HideDeadline)) {
        if (m_Pinned) {
            endLocalPointerInteraction();
            m_HideDeadline = 0;
        } else {
            hide();
        }
    }

    if (m_Visible && SDL_TICKS_PASSED(now, m_LastRedrawTime + RedrawIntervalMs) &&
            (std::fabs(m_RenderedFps - m_LastDrawnFps) >= 0.05f ||
             std::fabs(m_VideoMbps - m_LastDrawnVideoMbps) >= 0.05f)) {
        redraw();
    }

    queueBitrateRequest(now, false);
}

void StationConnectToolbar::notifyWindowChanged()
{
    SDL_GetWindowSize(m_Window, &m_WindowWidth, &m_WindowHeight);
    m_Width = std::min(ToolbarPreferredWidth, std::max(m_WindowWidth, 1));
    if (m_ToolbarLeft < 0) {
        m_ToolbarLeft = std::max(0, (m_WindowWidth - m_Width) / 2);
    } else {
        m_ToolbarLeft = qBound(0, m_ToolbarLeft,
                               std::max(0, m_WindowWidth - m_Width));
    }
    if (!m_PointerInitialized) {
        m_PointerX = m_WindowWidth / 2;
        m_PointerY = m_WindowHeight / 2;
        m_PointerInitialized = true;
    } else {
        m_PointerX = qBound(0, m_PointerX, std::max(0, m_WindowWidth - 1));
        m_PointerY = qBound(0, m_PointerY, std::max(0, m_WindowHeight - 1));
    }
    if (m_Visible) {
        redraw();
    }
}

bool StationConnectToolbar::handleMouseMotion(const SDL_MouseMotionEvent& event)
{
    if (event.which == SDL_TOUCH_MOUSEID) {
        return false;
    }

    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();
    // StationConnect sessions use absolute desktop input. SDL's window
    // coordinates are the single source of truth for both the remote pointer
    // and the receiver toolbar.
    m_PointerX = event.x;
    m_PointerY = event.y;

    // The edge must be held continuously for one second. Leaving the edge
    // cancels the activation, so ordinary host UI at the top remains usable.
    if (!m_Visible && !m_LocalPointerInteraction) {
        if (m_PointerY <= EdgeRevealHeight) {
            if (m_EdgeHoverStartTime == 0) {
                m_EdgeHoverStartTime = now;
            } else if (SDL_TICKS_PASSED(
                               now,
                               m_EdgeHoverStartTime + EdgeActivationDelayMs)) {
                show(now);
            }
        } else {
            m_EdgeHoverStartTime = 0;
        }
    }

    // Revealing does not claim the pointer. The user must move down from the
    // edge into the visible toolbar before local interaction begins.
    const bool pointerEnteredVisibleToolbar =
            m_Visible && m_PointerY > EdgeRevealHeight &&
            contains(m_PointerX, m_PointerY);
    if (!m_LocalPointerInteraction && pointerEnteredVisibleToolbar) {
        beginLocalPointerInteraction();
        return true;
    }

    if (!m_Visible) {
        return false;
    }

    m_PointerInside = contains(m_PointerX, m_PointerY);
    if (m_PointerInside || m_DraggingSlider) {
        m_HideDeadline = 0;
    } else {
        m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
    }

    // Match a native child toolbar's input routing: in absolute desktop mode,
    // receiver UI owns events only inside its exact rectangle (or during a
    // drag). Pinning changes visibility, never mouse capture or routing.
    if (m_LocalPointerInteraction && !m_DraggingToolbar &&
            !m_DraggingSlider && !m_PointerInside) {
        endLocalPointerInteraction();
        return false;
    }

    if (m_DraggingToolbar) {
        const int newLeft = qBound(0, m_PointerX - m_ToolbarDragOffsetX,
                                   std::max(0, m_WindowWidth - m_Width));
        if (newLeft != m_ToolbarLeft) {
            m_ToolbarLeft = newLeft;
            if (SDL_TICKS_PASSED(now,
                    m_LastToolbarMoveDrawTime + ToolbarMoveRedrawIntervalMs)) {
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

    if (m_LocalPointerInteraction) {
        redraw();
        return true;
    }
    return m_PointerInside;
}

StationConnectToolbar::Action StationConnectToolbar::handleMouseButton(
        const SDL_MouseButtonEvent& event)
{
    if (!m_Visible) {
        return Action::None;
    }

    m_PointerX = event.x;
    m_PointerY = event.y;
    const int pointerX = m_PointerX;
    const int pointerY = m_PointerY;
    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();
    if (event.button == SDL_BUTTON_LEFT && event.state == SDL_RELEASED &&
            m_DraggingToolbar) {
        m_DraggingToolbar = false;
        redraw();
        return Action::Consumed;
    }
    if (event.button == SDL_BUTTON_LEFT && event.state == SDL_RELEASED &&
            m_DraggingSlider) {
        updateBitrateFromPointer(pointerX, now, true);
        m_DraggingSlider = false;
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
        return Action::Consumed;
    }

    if (!contains(pointerX, pointerY)) {
        if (m_LocalPointerInteraction) {
            m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
            endLocalPointerInteraction();
        }
        return Action::None;
    }

    if (!m_LocalPointerInteraction) {
        beginLocalPointerInteraction();
    }

    if (event.button != SDL_BUTTON_LEFT) {
        return Action::Consumed;
    }

    if (event.state == SDL_PRESSED) {
        if (handleContains(pointerX, pointerY)) {
            m_DraggingToolbar = true;
            m_ToolbarDragOffsetX = pointerX - toolbarLeft();
            m_HideDeadline = 0;
            redraw();
        } else if (pinContains(pointerX, pointerY)) {
            m_Pinned = !m_Pinned;
            m_Preferences.stationConnectToolbarPinned = m_Pinned;
            m_Preferences.save();
            m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
            redraw();
        } else if (minimizeContains(pointerX, pointerY)) {
            endLocalPointerInteraction();
            return Action::Minimize;
        } else if (disconnectContains(pointerX, pointerY)) {
            return Action::Disconnect;
        } else if (m_BitrateSupported && sliderContains(pointerX, pointerY)) {
            m_DraggingSlider = true;
            updateBitrateFromPointer(pointerX, now, false);
        }
    }

    return Action::Consumed;
}

bool StationConnectToolbar::handleMouseWheel(const SDL_MouseWheelEvent& event)
{
    int mouseX;
    int mouseY;
    SDL_GetMouseState(&mouseX, &mouseY);
    m_PointerX = mouseX;
    m_PointerY = mouseY;
    if (!m_Visible) {
        return false;
    }
    if (!contains(mouseX, mouseY)) {
        endLocalPointerInteraction();
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
        queueBitrateRequest(event.timestamp != 0 ? event.timestamp : SDL_GetTicks(), true);
    }
    return true;
}

int StationConnectToolbar::eventWaitTimeout() const
{
    return (m_Visible || m_EdgeHoverStartTime != 0) ? 50 : 1000;
}

void StationConnectToolbar::show(Uint32 now)
{
    m_Visible = true;
    m_EdgeHoverStartTime = 0;
    m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
    redraw();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, true);
}

void StationConnectToolbar::hide()
{
    endLocalPointerInteraction();
    m_Visible = false;
    m_EdgeHoverStartTime = 0;
    m_PointerInside = false;
    m_HideDeadline = 0;
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
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
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
                0, m_Width, ToolbarHeight, 32, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to allocate StationConnect toolbar surface: %s",
                    SDL_GetError());
        return;
    }

    QImage image(static_cast<uchar*>(surface->pixels), surface->w, surface->h,
                 surface->pitch, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Paint the background through the first pixel row so the toolbar meets
    // the physical top edge without an antialiased one-pixel gap.
    painter.fillRect(QRect(0, 0, m_Width, ToolbarHeight - 4),
                     QColor(22, 27, 34, 238));
    painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(0.5, 0.5, m_Width - 1.0, ToolbarHeight - 5.0), 0, 0);

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
    painter.drawText(QRect(107, 5, 72, 12), Qt::AlignLeft | Qt::AlignVCenter,
                     "Video Mbps");
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(107, 16, 72, 17), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_VideoMbps, 'f', 1));

    QFont targetFont = labelFont;
    targetFont.setPixelSize(12);
    painter.setFont(targetFont);
    painter.setPen(m_BitrateSupported ? QColor(235, 239, 244) : QColor(135, 143, 153));
    painter.drawText(QRect(184, 3, 220, 17), Qt::AlignLeft | Qt::AlignVCenter,
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
        painter.drawText(QRect(184, 28, 187, 9), Qt::AlignLeft | Qt::AlignVCenter,
                         "Host update required for live control");
    }

    painter.end();
    m_LastDrawnFps = m_RenderedFps;
    m_LastDrawnVideoMbps = m_VideoMbps;
    m_LastRedrawTime = SDL_GetTicks();
    const int availableWidth = std::max(0, m_WindowWidth - m_Width);
    const float horizontalPosition = availableWidth > 0 ?
                static_cast<float>(m_ToolbarLeft) / availableWidth : 0.5f;
    m_OverlayManager.setOverlayHorizontalPosition(Overlay::OverlayToolbar,
                                                   horizontalPosition);
    m_OverlayManager.updateOverlaySurface(Overlay::OverlayToolbar, surface);
}

void StationConnectToolbar::updateBitrateFromPointer(int x, Uint32 now, bool forceSend)
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

void StationConnectToolbar::queueBitrateRequest(Uint32 now, bool forceSend)
{
    if (!m_BitrateSupported) {
        return;
    }
    if (m_BitrateKbps == m_AppliedBitrateKbps) {
        return;
    }
    if (!forceSend &&
            (!SDL_TICKS_PASSED(now, m_LastBitrateChangeTime + BitrateSettleDelayMs) ||
             !SDL_TICKS_PASSED(now, m_LastBitrateSendTime +
                              (m_BitrateKbps == m_LastSentBitrateKbps ?
                                   BitrateAcknowledgementRetryMs :
                                   BitrateSettleDelayMs)))) {
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
           y >= 0 && y <= ToolbarHeight - 5;
}

bool StationConnectToolbar::minimizeContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 69 &&
           x <= toolbarLeft() + m_Width - 41 && y >= 5 && y <= 33;
}

bool StationConnectToolbar::disconnectContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 36 &&
           x <= toolbarLeft() + m_Width - 8 && y >= 5 && y <= 33;
}

int StationConnectToolbar::toolbarLeft() const
{
    return std::max(0, m_ToolbarLeft);
}

int StationConnectToolbar::sliderLeft() const
{
    return toolbarLeft() + 184;
}

int StationConnectToolbar::sliderRight() const
{
    return toolbarLeft() + std::max(191, m_Width - 80);
}
