#include "stationconnecttoolbar.h"

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
constexpr int ToolbarPreferredWidth = 760;
constexpr int ToolbarHeight = 64;
constexpr int EdgeRevealHeight = 3;
constexpr Uint32 AutoHideDelayMs = 900;
constexpr Uint32 BitrateSendIntervalMs = 80;
constexpr Uint32 RedrawIntervalMs = 200;
constexpr int BitrateMinimumKbps = 500;
constexpr int BitrateStepKbps = 500;
}

StationConnectToolbar::StationConnectToolbar(
        SDL_Window* window,
        Overlay::OverlayManager& overlayManager,
        StreamingPreferences& preferences)
    : m_Window(window),
      m_OverlayManager(overlayManager),
      m_Preferences(preferences),
      m_Visible(preferences.stationConnectToolbarPinned),
      m_Pinned(preferences.stationConnectToolbarPinned),
      m_DraggingSlider(false),
      m_PointerInside(false),
      m_BitrateSupported((LiGetHostFeatureFlags() & LI_FF_DYNAMIC_VIDEO_BITRATE) != 0),
      m_WindowWidth(0),
      m_Width(ToolbarPreferredWidth),
      m_BitrateKbps(preferences.bitrateKbps),
      m_LastSentBitrateKbps(preferences.bitrateKbps),
      m_RenderedFps(0.0f),
      m_LastDrawnFps(-1.0f),
      m_HideDeadline(0),
      m_LastBitrateSendTime(0),
      m_LastRedrawTime(0)
{
    notifyWindowChanged();
    redraw();
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, m_Visible);
}

StationConnectToolbar::~StationConnectToolbar()
{
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
}

void StationConnectToolbar::setRenderedFps(float fps)
{
    m_RenderedFps = std::max(0.0f, fps);
}

void StationConnectToolbar::update(Uint32 now)
{
    if (m_Visible && !m_Pinned && !m_DraggingSlider && !m_PointerInside &&
            m_HideDeadline != 0 && SDL_TICKS_PASSED(now, m_HideDeadline)) {
        hide();
    }

    if (m_Visible && SDL_TICKS_PASSED(now, m_LastRedrawTime + RedrawIntervalMs) &&
            std::fabs(m_RenderedFps - m_LastDrawnFps) >= 0.05f) {
        redraw();
    }

    queueBitrateRequest(now, false);
}

void StationConnectToolbar::notifyWindowChanged()
{
    int windowHeight;
    SDL_GetWindowSize(m_Window, &m_WindowWidth, &windowHeight);
    m_Width = std::min(ToolbarPreferredWidth, std::max(m_WindowWidth, 1));
    if (m_Visible) {
        redraw();
    }
}

bool StationConnectToolbar::handleMouseMotion(const SDL_MouseMotionEvent& event)
{
    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();

    if (!m_Visible && event.y <= EdgeRevealHeight) {
        show(now);
    }

    if (!m_Visible) {
        return false;
    }

    m_PointerInside = contains(event.x, event.y);
    if (m_PointerInside || m_DraggingSlider) {
        m_HideDeadline = 0;
    } else if (!m_Pinned) {
        m_HideDeadline = now + AutoHideDelayMs;
    }

    if (m_DraggingSlider) {
        updateBitrateFromPointer(event.x, now, false);
        return true;
    }

    return m_PointerInside;
}

StationConnectToolbar::Action StationConnectToolbar::handleMouseButton(
        const SDL_MouseButtonEvent& event)
{
    if (!m_Visible || event.button != SDL_BUTTON_LEFT) {
        return Action::None;
    }

    const Uint32 now = event.timestamp != 0 ? event.timestamp : SDL_GetTicks();
    if (event.state == SDL_RELEASED && m_DraggingSlider) {
        updateBitrateFromPointer(event.x, now, true);
        m_DraggingSlider = false;
        m_Preferences.bitrateKbps = m_BitrateKbps;
        m_Preferences.save();
        return Action::Consumed;
    }

    if (!contains(event.x, event.y)) {
        return Action::None;
    }

    if (event.state == SDL_PRESSED) {
        if (pinContains(event.x, event.y)) {
            m_Pinned = !m_Pinned;
            m_Preferences.stationConnectToolbarPinned = m_Pinned;
            m_Preferences.save();
            m_HideDeadline = m_Pinned ? 0 : now + AutoHideDelayMs;
            redraw();
        } else if (disconnectContains(event.x, event.y)) {
            return Action::Disconnect;
        } else if (m_BitrateSupported && sliderContains(event.x, event.y)) {
            m_DraggingSlider = true;
            updateBitrateFromPointer(event.x, now, false);
        }
    }

    return Action::Consumed;
}

bool StationConnectToolbar::handleMouseWheel(const SDL_MouseWheelEvent& event)
{
    int mouseX;
    int mouseY;
    SDL_GetMouseState(&mouseX, &mouseY);
    if (!m_Visible || !m_BitrateSupported || !sliderContains(mouseX, mouseY)) {
        return false;
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
    m_Visible = false;
    m_PointerInside = false;
    m_HideDeadline = 0;
    m_OverlayManager.setOverlayState(Overlay::OverlayToolbar, false);
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

    // Pin control. Blue indicates the persistent pinned state.
    const QRect pinRect(10, 8, 42, 42);
    if (m_Pinned) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(52, 132, 228));
        painter.drawRoundedRect(pinRect, 7, 7);
    }
    painter.setPen(QPen(QColor(238, 242, 247), 2.0, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(24, 18, 38, 32);
    painter.drawLine(22, 28, 32, 18);
    painter.drawLine(28, 34, 40, 22);
    painter.drawLine(22, 38, 29, 31);

    painter.setPen(QColor(151, 161, 174));
    painter.drawText(QRect(62, 8, 34, 18), Qt::AlignLeft | Qt::AlignVCenter, "FPS");
    QFont valueFont = labelFont;
    valueFont.setPixelSize(20);
    painter.setFont(valueFont);
    painter.setPen(QColor(246, 248, 250));
    painter.drawText(QRect(62, 24, 105, 26), Qt::AlignLeft | Qt::AlignVCenter,
                     QString::number(m_RenderedFps, 'f', 1));

    painter.setFont(labelFont);
    painter.setPen(m_BitrateSupported ? QColor(235, 239, 244) : QColor(135, 143, 153));
    painter.drawText(QRect(166, 8, 150, 20), Qt::AlignLeft | Qt::AlignVCenter,
                     QString("Bitrate  %1 Mbps").arg(m_BitrateKbps / 1000.0, 0, 'f', 1));

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

    const QRect disconnectRect(m_Width - 158, 8, 146, 42);
    painter.setPen(QPen(QColor(239, 88, 88), 1));
    painter.setBrush(QColor(126, 35, 40, 185));
    painter.drawRoundedRect(disconnectRect, 7, 7);
    painter.setPen(QPen(QColor(255, 228, 228), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(m_Width - 143, 22, m_Width - 133, 32);
    painter.drawLine(m_Width - 133, 22, m_Width - 143, 32);
    painter.setFont(labelFont);
    painter.setPen(QColor(255, 235, 235));
    painter.drawText(QRect(m_Width - 125, 8, 106, 42),
                     Qt::AlignLeft | Qt::AlignVCenter, "Disconnect");

    if (!m_BitrateSupported) {
        QFont hintFont = labelFont;
        hintFont.setPixelSize(11);
        painter.setFont(hintFont);
        painter.setPen(QColor(183, 151, 92));
        painter.drawText(QRect(166, 42, 260, 14), Qt::AlignLeft | Qt::AlignVCenter,
                         "Host update required for live control");
    }

    painter.end();
    m_LastDrawnFps = m_RenderedFps;
    m_LastRedrawTime = SDL_GetTicks();
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
        redraw();
    }
    queueBitrateRequest(now, forceSend);
}

void StationConnectToolbar::queueBitrateRequest(Uint32 now, bool forceSend)
{
    if (!m_BitrateSupported || m_BitrateKbps == m_LastSentBitrateKbps) {
        return;
    }
    if (!forceSend && !SDL_TICKS_PASSED(now, m_LastBitrateSendTime + BitrateSendIntervalMs)) {
        return;
    }
    if (LiSetVideoBitrate(m_BitrateKbps) == 0) {
        m_LastSentBitrateKbps = m_BitrateKbps;
        m_LastBitrateSendTime = now;
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
    return x >= toolbarLeft() + 10 && x <= toolbarLeft() + 52 &&
           y >= 8 && y <= 50;
}

bool StationConnectToolbar::disconnectContains(int x, int y) const
{
    return x >= toolbarLeft() + m_Width - 158 &&
           x <= toolbarLeft() + m_Width - 12 && y >= 8 && y <= 50;
}

int StationConnectToolbar::toolbarLeft() const
{
    return std::max(0, (m_WindowWidth - m_Width) / 2);
}

int StationConnectToolbar::sliderLeft() const
{
    return toolbarLeft() + 166;
}

int StationConnectToolbar::sliderRight() const
{
    return toolbarLeft() + std::max(176, m_Width - 178);
}
