#pragma once

#include "sessiongeometry.h"
#include "cursoritem.h"

#include <QCursor>
#include <QMap>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QVariantMap>
#include <functional>

// Shared GUI-thread scene and input policy. An ordinary Qt window drives it on
// Linux; a render control can drive the same scene into an exact Metal target.
// No network call or decoder is owned by this component.
class QtSessionWindow : public QQuickWindow
{
    Q_OBJECT
    Q_PROPERTY(bool toolbarVisible READ toolbarVisible NOTIFY toolbarChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(bool canSpanDisplays READ canSpanDisplays NOTIFY displayModeChanged)
    Q_PROPERTY(bool spanningDisplays READ spanningDisplays NOTIFY displayModeChanged)
    Q_PROPERTY(bool displayMenuOpen READ displayMenuOpen WRITE setDisplayMenuOpen NOTIFY displayMenuChanged)
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY toolbarChanged)
    Q_PROPERTY(qreal toolbarPosition READ toolbarPosition NOTIFY toolbarChanged)
    Q_PROPERTY(bool waiting READ waiting WRITE setWaiting NOTIFY waitingChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(int minimumBitrateKbps READ minimumBitrateKbps CONSTANT)
    Q_PROPERTY(int maximumBitrateKbps READ maximumBitrateKbps CONSTANT)
    Q_PROPERTY(int bitrateStepKbps READ bitrateStepKbps CONSTANT)
    Q_PROPERTY(QRectF videoRect READ videoRect NOTIFY presentationChanged)
    Q_PROPERTY(bool liveSession READ liveSession WRITE setLiveSession NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText WRITE setStatusText NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap telemetry READ telemetry WRITE setTelemetry NOTIFY telemetryChanged)
    Q_PROPERTY(QString statisticsText READ statisticsText WRITE setStatisticsText NOTIFY statisticsChanged)
    Q_PROPERTY(bool statisticsVisible READ statisticsVisible WRITE setStatisticsVisible NOTIFY statisticsChanged)
    Q_PROPERTY(bool immersiveMouse READ immersiveMouse NOTIFY immersiveMouseChanged)
    Q_PROPERTY(bool immersiveMouseAvailable READ immersiveMouseAvailable NOTIFY immersiveMouseChanged)
public:
    explicit QtSessionWindow(QWindow* parent = nullptr, QQuickRenderControl* renderControl = nullptr);
    ~QtSessionWindow() override;

    QQuickItem* rootObject() const { return m_Root; }
    QQmlComponent::Status status() const { return m_Component.status(); }

    bool toolbarVisible() const { return m_ToolbarVisible; }
    bool fullscreen() const { return m_Fullscreen; }
    bool canSpanDisplays() const { return m_CanSpanDisplays; }
    bool spanningDisplays() const { return m_SpanningDisplays; }
    bool displayMenuOpen() const { return m_DisplayMenuOpen; }
    void setDisplayMode(bool canSpan, bool spanning);
    void setDisplayMenuOpen(bool open);
    bool pinned() const { return m_Pinned; }
    qreal toolbarPosition() const { return m_ToolbarPosition; }
    bool waiting() const { return m_Waiting; }
    int bitrateKbps() const { return m_BitrateKbps; }
    int minimumBitrateKbps() const;
    int maximumBitrateKbps() const;
    int bitrateStepKbps() const;
    QRectF videoRect() const;
    bool liveSession() const { return m_LiveSession; }
    QString statusText() const { return m_StatusText; }
    void setLiveSession(bool live) { m_LiveSession = live; emit statusChanged(); }
    void setStatusText(const QString& text) { if (text != m_StatusText) { m_StatusText = text; emit statusChanged(); } }
    QVariantMap telemetry() const { return m_Telemetry; }
    void setTelemetry(const QVariantMap& values) { if (m_Telemetry != values) { m_Telemetry = values; emit telemetryChanged(); } }
    QString statisticsText() const { return m_StatisticsText; }
    void setStatisticsText(const QString& text) { m_StatisticsText = text; emit statisticsChanged(); }
    bool statisticsVisible() const { return m_StatisticsVisible; }
    void setStatisticsVisible(bool visible) { m_StatisticsVisible = visible; emit statisticsChanged(); }
    void setCursorShape(QImage image, QPoint hotspot, bool visible);
    void setHostCursorPosition(QPointF position);
    void setTabletActive(bool active);
    void setEmbeddedCursor(bool embedded);
    void setPointerCaptured(bool captured) {
        m_PointerCaptured = captured;
        if (captured) setToolbarVisible(false); else if (m_Pinned) setToolbarVisible(true);
        updateCursor();
    }
    bool immersiveMouse() const { return m_ImmersiveMouse; }
    bool immersiveMouseAvailable() const { return m_ImmersiveMouseAvailable; }
    void setImmersiveMouse(bool enabled, bool available) {
        m_ImmersiveMouse = enabled; m_ImmersiveMouseAvailable = available; emit immersiveMouseChanged();
    }
    Q_INVOKABLE void toggleImmersiveMouse() { emit immersiveMouseRequested(); }
    void discardPointerMotionBefore(quint64 timestamp) { m_PointerTimestampBoundary = timestamp; }
    bool reanchorPointer(QPointF framePosition);

    void setPresentation(const QtSessionGeometry& geometry);
    const QtSessionGeometry& presentation() const { return m_Geometry; }
    // During a compositor mouse grab, coordinates can leave this window. The
    // session maps that event through the visible outputs' existing viewports.
    void setDragMapper(std::function<bool(QPointF, QPointF&)> mapper) { m_DragMapper = std::move(mapper); }
    void setNativeKeyCapture(std::function<bool()> captured) { m_NativeKeyCapture = std::move(captured); }
    void setRemoteInputEnabled(bool enabled);
#ifdef Q_OS_MACOS
    void cancelPen();
#endif
    void setRemoteCursor(const QCursor& cursor);
    void setPinned(bool pinned);
    void setWaiting(bool waiting);
    void setBitrateKbps(int kbps);
    void cancelInput();
    Q_INVOKABLE void moveToolbar(qreal logicalLeft);
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void fullscreenDisplays(bool span) { cancelInput(); emit fullscreenDisplaysRequested(span); }
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void openNewConnection();
    Q_INVOKABLE void disconnectSession();
    Q_INVOKABLE void toggleMicrophone() { emit microphoneRequested(); }
    Q_INVOKABLE void toggleCamera() { emit cameraRequested(); }
    Q_INVOKABLE void toggleStatistics() { setStatisticsVisible(!statisticsVisible()); }

signals:
    void statusChanged();
    void telemetryChanged();
    void statisticsChanged();
    void physicalMouseActivity();
    void inputRecaptureRequested();
    void fullscreenRequested();
    void fullscreenChanged();
    void displayModeChanged();
    void displayMenuChanged();
    void fullscreenDisplaysRequested(bool span);
    void minimizeRequested();
    void newConnectionRequested();
    void microphoneRequested();
    void cameraRequested();
    void toolbarChanged();
    void waitingChanged();
    void bitrateChanged();
    void presentationChanged();
    // Receiver recomputes the actual viewport before enabling input again.
    void surfaceSizeChanged();
    void remoteMotion(QPointF streamPosition);
    void remoteButton(int button, bool down, QPointF streamPosition);
    void remoteWheel(QPointF streamPosition, QPoint angleDelta, QPoint pixelDelta, bool inverted);
    void remoteKey(quint32 scanCode, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers);
    void remotePen(QPointF position, float pressure, float rotation, int xTilt, int yTilt, bool eraser, int buttons);
    void remotePenCancelled();
    void immersiveMouseRequested();
    void immersiveMouseChanged();
    void bitrateRequested(int kbps);
    void disconnectRequested();

protected:
    bool event(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;

private:
    bool overToolbar(const QPointF& position) const;
    bool localPointer(const QPointF& position) const;
    void observePointer(const QPointF& position);
    void setToolbarVisible(bool visible);
    void updateCursor();
    void updateTabletCursor();
    void scheduleHide();
    void releaseRemoteInput();
    void synchronizeSceneSize();
    bool remoteKeyboard() const;
    static quint64 keyIdentity(const QKeyEvent* event);
    QWindow* controlWindow();
    bool mapPointer(QPointF logical, QPointF& stream, bool drag) const;

    QQmlEngine m_Engine;
    QQmlComponent m_Component{&m_Engine};
    QQuickItem* m_Root = nullptr;
    QSize m_ObservedSize;
    QPointer<QWindow> m_ControlWindow;
    const bool m_Redirected;
    QtSessionGeometry m_Geometry;
    bool m_CanSpanDisplays = false;
    bool m_SpanningDisplays = false;
    bool m_DisplayMenuOpen = false;
    std::function<bool(QPointF, QPointF&)> m_DragMapper;
    std::function<bool()> m_NativeKeyCapture;
    QPointer<QQuickItem> m_Toolbar;
    QTimer m_RevealTimer;
    QTimer m_HideTimer;
    QCursor m_RemoteCursor;
    QPointF m_Pointer;
    QPointF m_LastRemotePosition;
    Qt::MouseButtons m_RemoteButtons;
    Qt::MouseButtons m_LocalButtons;
    struct Key { quint32 scanCode; int key; Qt::KeyboardModifiers modifiers; };
    QMap<quint64, Key> m_RemoteKeys;
    bool m_RemoteInputEnabled = false;
    bool m_PointerPresent = false;
    bool m_ToolbarVisible = false;
    bool m_Fullscreen = false;
    bool m_Pinned = false;
    bool m_Waiting = false;
    qreal m_ToolbarPosition = 0.5;
    int m_BitrateKbps;
    bool m_LiveSession = false;
    QString m_StatusText;
    QVariantMap m_Telemetry;
    QString m_StatisticsText;
    bool m_StatisticsVisible = false;
    QtTabletCursorItem* m_TabletCursor = nullptr;
    QImage m_CursorImage;
    QPoint m_CursorHotspot;
    QPointF m_HostCursorPosition;
    bool m_HostPositionValid = false;
    bool m_HostCursorVisible = true;
    bool m_TabletActive = false;
    bool m_EmbeddedCursor = false;
    bool m_PointerCaptured = false;
    bool m_ReanchorPending = false;
    QPointF m_ReanchorPosition;
    quint64 m_PointerTimestampBoundary = 0;
    bool m_ImmersiveMouse = false, m_ImmersiveMouseAvailable = false;
#ifdef Q_OS_MACOS
    bool m_RemotePenDown = false;
#endif
};
