#pragma once

#include <QBackingStore>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>
#include <QThread>
#include <memory>
#include <mutex>

// A presentation-only marker for the Host's real desktop pointer. It never
// warps the local pointer or participates in mouse/keyboard hit testing.
inline QImage plankSharedPointerImage()
{
    QImage image(78, 35, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath arrow;
    arrow.moveTo(1, 1); arrow.lineTo(5, 24); arrow.lineTo(11, 17);
    arrow.lineTo(18, 28); arrow.lineTo(23, 25); arrow.lineTo(16, 14);
    arrow.lineTo(26, 13); arrow.closeSubpath();
    painter.setPen(QPen(QColor(12, 30, 44), 2));
    painter.setBrush(QColor(83, 213, 255));
    painter.drawPath(arrow);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 30, 44, 235));
    painter.drawRoundedRect(QRectF(27, 14, 50, 20), 5, 5);
    QFont font = QGuiApplication::font(); font.setPixelSize(11);
    painter.setFont(font); painter.setPen(Qt::white);
    painter.drawText(QRect(27, 14, 50, 20), Qt::AlignCenter, QStringLiteral("Remote"));
    return image;
}

// The marker is hidden while local input is moving and while the host cursor
// overlaps it, avoiding a second lagging cursor during ordinary solo use.
inline bool plankSharedPointerVisible(bool focused, bool tablet, bool remoteVisible,
                                      bool viewOnly, bool sameWindow, QPointF remote,
                                      QPointF local, quint64 idleMs)
{
    return focused && !tablet && remoteVisible &&
        (viewOnly || !sameWindow || (idleMs >= 120 &&
            (qAbs(remote.x() - local.x()) > 12 || qAbs(remote.y() - local.y()) > 12)));
}

// Raster fallback for Windows/X11. Cocoa uses a hit-test-transparent child
// view; Wayland uses the existing input-less cursor subsurface instead.
class PlankSharedPointerWindow : public QWindow
{
public:
    PlankSharedPointerWindow() : m_Store(this), m_Image(plankSharedPointerImage())
    {
        setObjectName(QStringLiteral("plankSharedPointer"));
        setFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                 Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnTopHint);
        QSurfaceFormat format; format.setAlphaBufferSize(8); setFormat(format);
        resize(m_Image.size());
    }
    void present(QPoint global, bool visible)
    {
        if (!visible) { hide(); return; }
        if (auto* screen = QGuiApplication::screenAt(global)) setScreen(screen);
        setPosition(global);
        show();
        requestUpdate();
    }
protected:
    bool event(QEvent* event) override
    {
        const bool result = QWindow::event(event);
        if ((event->type() == QEvent::Expose || event->type() == QEvent::UpdateRequest) && isExposed()) {
            m_Store.resize(size());
            m_Store.beginPaint(QRect(QPoint(), size()));
            QPainter painter(m_Store.paintDevice());
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(QRect(QPoint(), size()), Qt::transparent);
            painter.drawImage(QPoint(), m_Image);
            painter.end();
            m_Store.endPaint();
            m_Store.flush(QRect(QPoint(), size()));
        }
        return result;
    }
private:
    QBackingStore m_Store;
    QImage m_Image;
};

// SDL runs on a worker on Linux. Coalesce positions and create/destroy raster
// windows only on Qt's GUI thread; stale queued updates cannot resurrect one
// after input teardown. No presentation object crosses the thread boundary.
class PlankSharedPointerOverlay
{
    struct Delivery {
        std::mutex mutex;
        QPoint point;
        bool visible = false, queued = false, alive = true;
        PlankSharedPointerWindow* window = nullptr; // GUI thread only
    };
public:
    PlankSharedPointerOverlay() : m_Delivery(std::make_shared<Delivery>()) {}
    ~PlankSharedPointerOverlay() { update({}, false, false); }
    void hide() { update({}, false, true); }
    void present(QPoint point, bool visible) { update(point, visible, true); }
private:
    void update(QPoint point, bool visible, bool alive)
    {
        const auto state = m_Delivery;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->point = point; state->visible = visible; state->alive = alive;
            if (state->queued) return;
            state->queued = true;
        }
        QMetaObject::invokeMethod(QGuiApplication::instance(), [state] {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto point = state->point;
            const bool visible = state->visible, alive = state->alive;
            state->queued = false;
            if (!alive) { delete state->window; state->window = nullptr; return; }
            if (!visible) { if (state->window) state->window->hide(); return; }
            if (!state->window) state->window = new PlankSharedPointerWindow();
            state->window->present(point, true);
        }, Qt::QueuedConnection);
    }
    std::shared_ptr<Delivery> m_Delivery;
};
