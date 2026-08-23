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
constexpr int ToolbarPreferredWidth = 850;
constexpr int ToolbarHeight = 64;
constexpr int EdgeRevealHeight = 3;
constexpr int InteractionExitY = ToolbarHeight + 32;
constexpr Uint32 AutoHideDelayMs = 1400;
constexpr Uint32 BitrateSettleDelayMs = 250;
constexpr Uint32 RedrawIntervalMs = 200;
constexpr Uint32 ToolbarMoveRedrawIntervalMs = 16;
constexpr int BitrateMinimumKbps = 500;
constexpr int BitrateStepKbps = 500;
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
      m_RestoreCapture(false),
      m_ConsumeNextLeftRelease(false),
      m_BitrateSupported((LiGetHostFeatureFlags() & LI_FF_DYNAMIC_VIDEO_BITRATE) != 0),
      m_WindowWidth(0),
      m_WindowHeight(0),
      m_Width(ToolbarPreferredWidth),
      m_ToolbarLeft(-1),
      m_ToolbarDragOffsetX(0),
      m_PointerX(0),
      m_PointerY(0),
      m_BitrateKbps(preferences.bitrateKbps),
      m_LastSentBitrateKbps(-1),
      m_RenderedFps(0.0f),
      m_VideoMbps(0.0f),
      m_LastDrawnFps(-1.0f),
      m_LastDrawnVideoMbps(-1.0f),
      m_HideDeadline(0),
      m_LastBitrateSendTime(0),
      m_LastBitrateChangeTime(0),
      m_LastToolbarMoveDrawTime(0),
      m_LastRedrawTime(0)
{
    notifyWindowChanged();
    redraw();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, m_Visible);
    // The normal RTSP setup reserves part of Moonlight's configured network
    // budget for FEC and audio. The in-session control is explicitly an
    // encoder target, so send it once at startup to make the toolbar value and
    // the fresh bounded-ABR encoder agree.
    queueBitrateRequest(SDL_GetTicks(), true);
}

StationConnectToolbar::~StationConnectToolbar()
{
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
}

void StationConnectToolbar::setRenderedStats(float fps, float videoMbps)
{
    m_RenderedFps = std::max(0.0f, fps);
    m_VideoMbps = std::max(0.0f, videoMbps);
}

void StationConnectToolbar::update(Uint32 now)
{
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
    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();
    const bool relativeMode = SDL_GetRelativeMouseMode();
    if (relativeMode) {
        // Track a private position only long enough to detect the top edge.
        // Once revealed, the toolbar releases relative capture and uses the
        // real local pointer so its hit testing always agrees with the cursor.
        m_PointerX = qBound(0, m_PointerX + event.xrel,
                           std::max(0, m_WindowWidth - 1));
        m_PointerY = qBound(0, m_PointerY + event.yrel,
                           std::max(0, m_WindowHeight - 1));
    } else {
        m_PointerX = event.x;
        m_PointerY = event.y;
    }

    if (m_PointerY <= EdgeRevealHeight && !m_LocalPointerInteraction) {
        if (!m_Visible) {
            show(now);
        }
        beginLocalPointerInteraction(now);
        return true;
    }

    if (!m_Visible) {
        return false;
    }

    // A pinned toolbar stays visible while normal remote relative-mouse input
    // continues. It only consumes input after the pointer reaches the edge.
    if (!m_LocalPointerInteraction && relativeMode) {
        return false;
    }

    m_PointerInside = contains(m_PointerX, m_PointerY);
    if (m_PointerInside || m_DraggingSlider) {
        m_HideDeadline = 0;
    } else {
        m_HideDeadline = now + AutoHideDelayMs;
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

    if (m_LocalPointerInteraction && !m_DraggingSlider &&
            m_PointerY > InteractionExitY) {
        if (m_Pinned) {
            endLocalPointerInteraction();
        } else {
            hide();
        }
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
    if (event.button == SDL_BUTTON_LEFT && event.state == SDL_RELEASED &&
            m_ConsumeNextLeftRelease) {
        m_ConsumeNextLeftRelease = false;
        return Action::Consumed;
    }

    if (!m_Visible) {
        return Action::None;
    }

    if (event.button != SDL_BUTTON_LEFT) {
        return m_LocalPointerInteraction ? Action::Consumed : Action::None;
    }

    const int pointerX = m_LocalPointerInteraction ? event.x : m_PointerX;
    const int pointerY = m_LocalPointerInteraction ? event.y : m_PointerY;
    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();
    if (event.state == SDL_RELEASED && m_DraggingToolbar) {
        m_DraggingToolbar = false;
        redraw();
        return Action::Consumed;
    }
    if (event.state == SDL_RELEASED && m_DraggingSlider) {
        updateBitrateFromPointer(pointerX, now, true);
        m_DraggingSlider = false;
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
        return Action::Consumed;
    }

    if (!contains(pointerX, pointerY)) {
        if (m_LocalPointerInteraction && event.state == SDL_PRESSED) {
            m_ConsumeNextLeftRelease = true;
            if (m_Pinned) {
                endLocalPointerInteraction();
            } else {
                hide();
            }
            return Action::Consumed;
        }
        return m_LocalPointerInteraction ? Action::Consumed : Action::None;
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
    if (m_LocalPointerInteraction) {
        SDL_GetMouseState(&mouseX, &mouseY);
    } else if (SDL_GetRelativeMouseMode()) {
        mouseX = m_PointerX;
        mouseY = m_PointerY;
    } else {
        SDL_GetMouseState(&mouseX, &mouseY);
    }
    if (!m_Visible || !m_BitrateSupported || !sliderContains(mouseX, mouseY)) {
        return m_LocalPointerInteraction;
    }

    const int delta = event.y > 0 ? BitrateStepKbps :
                      event.y < 0 ? -BitrateStepKbps : 0;
    if (delta != 0) {
        const int maximum = m_Preferences.unlockBitrate ? 500000 : 150000;
        m_BitrateKbps = qBound(BitrateMinimumKbps,
                               m_BitrateKbps + delta, maximum);
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
        redraw();
        queueBitrateRequest(event.timestamp != 0 ? event.timestamp : SDL_GetTicks(), true);
    }
    return true;
}

int StationConnectToolbar::eventWaitTimeout() const
{
    return m_Visible ? 50 : 1000;
}

void StationConnectToolbar::show(Uint32 now)
{
    m_Visible = true;
    m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
    redraw();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, true);
}

void StationConnectToolbar::hide()
{
    endLocalPointerInteraction();
    m_Visible = false;
    m_PointerInside = false;
    m_HideDeadline = 0;
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
}

void StationConnectToolbar::beginLocalPointerInteraction(Uint32 now)
{
    if (m_LocalPointerInteraction) {
        return;
    }

    m_RestoreCapture = m_InputHandler.isCaptureActive();
    if (m_RestoreCapture) {
        m_InputHandler.setCaptureActive(false);
    }
    SDL_ShowCursor(SDL_ENABLE);

    m_LocalPointerInteraction = true;
    m_PointerX = qBound(toolbarLeft() + 2, m_PointerX,
                        toolbarLeft() + m_Width - 3);
    m_PointerY = qBound(2, m_PointerY, ToolbarHeight - 3);
    SDL_WarpMouseInWindow(m_Window, m_PointerX, m_PointerY);
    m_PointerInside = true;
    m_HideDeadline = 0;
    redraw();
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "StationConnect toolbar entered local pointer interaction");
    Q_UNUSED(now);
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
    if (m_RestoreCapture) {
        m_InputHandler.setCaptureActive(true);
    }
    m_RestoreCapture = false;
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                 "StationConnect toolbar restored remote pointer interaction");
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
    painter.setPen(QPen(QColor(255, 255, 255, 42), 1));
    painter.setBrush(QColor(22, 27, 34, 238));
    painter.drawRoundedRect(QRectF(0.5, 0.5, m_Width - 1.0, ToolbarHeight - 8.0), 0, 0);

    QFont labelFont;
    labelFont.setPixelSize(15);
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);
    painter.setPen(QColor(235, 239, 244));

    // Six-dot grip for moving the toolbar horizontally along the top edge.
    const bool handleHovered = m_LocalPointerInteraction &&
                               handleContains(m_PointerX, m_PointerY);
    painter.setPen(Qt::NoPen);
    painter.setBrush(handleHovered || m_DraggingToolbar ?
                         QColor(205, 214, 224) : QColor(123, 134, 147));
    for (int y : {22, 29, 36}) {
        painter.drawEllipse(QPointF(14, y), 1.8, 1.8);
        painter.drawEllipse(QPointF(21, y), 1.8, 1.8);
    }

    // RGS-style upright outline thumbtack. Blue indicates the pinned state.
    const QRect pinRect(34, 8, 42, 42);
    const bool pinHovered = m_LocalPointerInteraction &&
                            pinContains(m_PointerX, m_PointerY);
    if (m_Pinned || pinHovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_Pinned ? QColor(52, 132, 228) : QColor(69, 78, 89));
        painter.drawRoundedRect(pinRect, 7, 7);
    }
    QPainterPath pin;
    pin.moveTo(46, 17);
    pin.lineTo(64, 17);
    pin.lineTo(60, 22);
    pin.lineTo(60, 29);
    pin.lineTo(65, 34);
    pin.lineTo(56.5, 34);
    pin.lineTo(55, 43);
    pin.lineTo(53.5, 34);
    pin.lineTo(45, 34);
    pin.lineTo(50, 29);
    pin.lineTo(50, 22);
    pin.closeSubpath();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(238, 242, 247), 1.8, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(pin);

    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(86, 8, 50, 18), Qt::AlignLeft | Qt::AlignVCenter, "FPS");
    QFont valueFont = labelFont;
    valueFont.setPixelSize(20);
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(86, 24, 74, 26), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_RenderedFps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(160, 8, 108, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     "VIDEO Mbps");
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(160, 24, 108, 26), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_VideoMbps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(m_BitrateSupported ? QColor(235, 239, 244) : QColor(135, 143, 153));
    painter.drawText(QRect(276, 8, 230, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     QString("Encoder target  %1 Mbps").arg(m_BitrateKbps / 1000.0, 0, 'f', 1));

    const int trackLeft = sliderLeft() - toolbarLeft();
    const int trackRight = sliderRight() - toolbarLeft();
    const int trackY = 40;
    painter.setPen(QPen(QColor(92, 102, 114), 5, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(trackLeft, trackY, trackRight, trackY);

    const int maximum = m_Preferences.unlockBitrate ? 500000 : 150000;
    const qreal fraction = qreal(m_BitrateKbps - BitrateMinimumKbps) /
                           qreal(maximum - BitrateMinimumKbps);
    const int thumbX = trackLeft + qRound(fraction * (trackRight - trackLeft));
    if (m_BitrateSupported) {
        painter.setPen(QPen(QColor(52, 132, 228), 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(trackLeft, trackY, thumbX, trackY);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_BitrateSupported ? QColor(243, 246, 250) : QColor(112, 120, 130));
    painter.drawEllipse(QPointF(thumbX, trackY), 7, 7);

    const QRect minimizeRect(m_Width - 104, 8, 42, 42);
    const bool minimizeHovered = m_LocalPointerInteraction &&
                                 minimizeContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(104, 116, 131), 1));
    painter.setBrush(minimizeHovered ? QColor(68, 78, 90, 230) :
                                      QColor(48, 57, 68, 210));
    painter.drawRoundedRect(minimizeRect, 7, 7);
    painter.setPen(QPen(QColor(226, 232, 239), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(m_Width - 91, 31, m_Width - 75, 31);

    const QRect disconnectRect(m_Width - 54, 8, 42, 42);
    const bool disconnectHovered = m_LocalPointerInteraction &&
                                   disconnectContains(m_PointerX, m_PointerY);
    painter.setPen(QPen(QColor(239, 88, 88), 1));
    painter.setBrush(disconnectHovered ? QColor(151, 43, 49, 220) :
                                        QColor(126, 35, 40, 185));
    painter.drawRoundedRect(disconnectRect, 7, 7);
    painter.setPen(QPen(QColor(255, 228, 228), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(m_Width - 42, 20, m_Width - 24, 38);
    painter.drawLine(m_Width - 24, 20, m_Width - 42, 38);

    if (!m_BitrateSupported) {
        QFont hintFont = labelFont;
        hintFont.setPixelSize(11);
        painter.setFont(hintFont);
        painter.setPen(QColor(183, 151, 92));
        painter.drawText(QRect(276, 42, 280, 14), Qt::AlignLeft | Qt::AlignVCenter,
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
    const int maximum = m_Preferences.unlockBitrate ? 500000 : 150000;
    const qreal fraction = qBound(
                qreal(0.0),
                qreal(x - sliderLeft()) / qreal(std::max(1, sliderRight() - sliderLeft())),
                qreal(1.0));
    int bitrate = BitrateMinimumKbps +
                  qRound(fraction * (maximum - BitrateMinimumKbps));
    bitrate = qRound(qreal(bitrate) / BitrateStepKbps) * BitrateStepKbps;
    bitrate = qBound(BitrateMinimumKbps, bitrate, maximum);
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
    if (!m_BitrateSupported || m_BitrateKbps == m_LastSentBitrateKbps) {
        return;
    }
    if (!forceSend &&
            (!SDL_TICKS_PASSED(now, m_LastBitrateChangeTime + BitrateSettleDelayMs) ||
             !SDL_TICKS_PASSED(now, m_LastBitrateSendTime + BitrateSettleDelayMs))) {
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
    return x >= sliderLeft() - 10 && x <= sliderRight() + 10 &&
           y >= 25 && y <= 55;
}

bool StationConnectToolbar::pinContains(int x, int y) const
{
    return x >= toolbarLeft() + 34 && x <= toolbarLeft() + 76 &&
           y >= 8 && y <= 50;
}

bool StationConnectToolbar::handleContains(int x, int y) const
{
    return x >= toolbarLeft() + 6 && x <= toolbarLeft() + 29 &&
           y >= 8 && y <= 50;
}

bool StationConnectToolbar::minimizeContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 104 &&
           x <= toolbarLeft() + m_Width - 62 && y >= 8 && y <= 50;
}

bool StationConnectToolbar::disconnectContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 54 &&
           x <= toolbarLeft() + m_Width - 12 && y >= 8 && y <= 50;
}

int StationConnectToolbar::toolbarLeft() const
{
    return std::max(0, m_ToolbarLeft);
}

int StationConnectToolbar::sliderLeft() const
{
    return toolbarLeft() + 276;
}

int StationConnectToolbar::sliderRight() const
{
    return toolbarLeft() + std::max(286, m_Width - 120);
}
