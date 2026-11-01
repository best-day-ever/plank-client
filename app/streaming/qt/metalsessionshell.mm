#include "metalsessionshell.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <QCoreApplication>
#include <QDebug>
#include <QDynamicPropertyChangeEvent>
#include <QQuickGraphicsDevice>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QScreen>
#include <QSGRendererInterface>
#include <QThread>
#include <condition_variable>
#include <mutex>

namespace {
CAMetalLayer* metalLayer(CALayer* root)
{
    if ([root isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)root;
    // Cocoa may wrap the rendering layer in a layout/container layer. Follow
    // public Core Animation children rather than depending on Qt's private type.
    for (CALayer* child in root.sublayers)
        if (auto* layer = metalLayer(child)) return layer;
    return nil;
}

class RenderControl final : public QQuickRenderControl
{
public:
    explicit RenderControl(QWindow* window) : m_Window(window) {}
    QWindow* renderWindow(QPoint* offset) override
    {
        if (offset) *offset = {};
        return m_Window;
    }
private:
    QWindow* const m_Window;
};
}

struct QtMetalSessionShell::Rendering
{
    explicit Rendering(QtMetalSessionShell* owner) : shell(owner), control(owner) {}
    ~Rendering()
    {
        Q_ASSERT(!thread || thread->isFinished());
        delete thread;
        if (idleAssertion != kIOPMNullAssertionID) IOPMAssertionRelease(idleAssertion);
        [layer release]; [queue release]; [device release];
    }
    QtMetalSessionShell* const shell;
    RenderControl control;
    QThread* thread = nullptr;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    IOPMAssertionID idleAssertion = kIOPMNullAssertionID;
    std::mutex mutex;
    std::condition_variable changed;
    bool stop = false, requested = false, sync = false, synced = false, cancel = false;
    QSize pixels;
    qreal dpr = 1;

    void fail(QString reason)
    {
        qWarning().noquote() << "Qt Metal session:" << reason;
        QMetaObject::invokeMethod(shell, [owner = shell, reason] {
            emit owner->renderingFailed(reason);
        }, Qt::QueuedConnection);
    }

    void run()
    {
        @autoreleasepool {
            auto& scene = shell->surface();
            if (!control.initialize()) {
                fail(QStringLiteral("Could not initialize the Qt Metal scene"));
                return;
            }
            id<MTLCommandBuffer> submission = nil;
            // Qt exposes the native command buffer while recording the frame,
            // then clears that public resource before render() returns. Capture
            // it at the documented render signal, before endFrame() commits it.
            const auto recording = QObject::connect(&scene, &QQuickWindow::afterRendering, &scene,
                [&scene, &submission] {
                    submission = (id<MTLCommandBuffer>)scene.rendererInterface()->getResource(
                        &scene, QSGRendererInterface::CommandListResource);
                }, Qt::DirectConnection);
            for (;;) {
                QSize framePixels;
                qreal frameDpr;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [this] { return stop || requested; });
                    if (stop) break;
                    requested = false;
                    framePixels = pixels; frameDpr = dpr;
                }
                @autoreleasepool {
                    // Drawable acquisition and GPU completion may wait. Both
                    // happen here, never while the GUI is waiting for sync().
                    id<CAMetalDrawable> drawable = [layer nextDrawable];
                    if (!drawable) {
                        // Covering/minimizing a window can temporarily retire
                        // its drawable pool. Exposure/next video delivery will
                        // request another frame; it is not a transport failure.
                        QMetaObject::invokeMethod(shell, &QtMetalSessionShell::frameFinished, Qt::QueuedConnection);
                        continue;
                    }
                    const auto texture = drawable.texture;
                    if (texture.pixelFormat != MTLPixelFormatRGB10A2Unorm) {
                        fail(QStringLiteral("The Qt Metal drawable changed pixel format (%1)").arg(qulonglong(texture.pixelFormat)));
                        break;
                    }
                    if (texture.width != NSUInteger(framePixels.width()) ||
                            texture.height != NSUInteger(framePixels.height())) {
                        // Native resize may win the race with acquisition. A
                        // later scene-size update will submit matching geometry.
                        QMetaObject::invokeMethod(shell, &QtMetalSessionShell::frameFinished, Qt::QueuedConnection);
                        continue;
                    }
                    auto target = QQuickRenderTarget::fromMetalTexture((MTLTexture*)texture,
                        uint(MTLPixelFormatRGB10A2Unorm), framePixels);
                    target.setDevicePixelRatio(frameDpr);
                    scene.setRenderTarget(target);
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        if (stop) break;
                        sync = false; synced = false; cancel = false;
                        QMetaObject::invokeMethod(shell, &QtMetalSessionShell::synchronizeFrame, Qt::QueuedConnection);
                        changed.wait(lock, [this] { return stop || sync || cancel; });
                        if (stop) break;
                        if (cancel) {
                            QMetaObject::invokeMethod(shell, &QtMetalSessionShell::frameFinished, Qt::QueuedConnection);
                            continue;
                        }
                    }
                    control.beginFrame();
                    control.sync();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        synced = true;
                    }
                    changed.notify_all(); // GUI can dispatch immediately again.
                    submission = nil;
                    control.render();
                    auto command = submission;
                    if (command) [command presentDrawable:drawable];
                    control.endFrame(); // Qt waits for GPU completion on this worker.
                    if (!command || command.status == MTLCommandBufferStatusError) {
                        fail(QStringLiteral("The Qt Metal presentation command failed: %1").arg(
                            command ? QString::fromNSString(command.error.description) : QStringLiteral("no command buffer")));
                        break;
                    }
                    emit scene.frameSwapped();
                    QMetaObject::invokeMethod(shell, &QtMetalSessionShell::frameFinished, Qt::QueuedConnection);
                }
            }
            QObject::disconnect(recording);
            control.invalidate();
        }
    }
};

QtMetalSessionShell::QtMetalSessionShell()
    : m_Rendering(std::make_unique<Rendering>(this)),
      m_Surface(std::make_unique<QtSessionWindow>(this, &m_Rendering->control))
{
    setSurfaceType(QSurface::MetalSurface);
    setTitle(QStringLiteral("PLANK"));
    m_Surface->installEventFilter(this);
    connect(&m_Rendering->control, &QQuickRenderControl::renderRequested,
            this, &QtMetalSessionShell::requestRender);
    connect(&m_Rendering->control, &QQuickRenderControl::sceneChanged,
            this, &QtMetalSessionShell::requestRender);
    connect(this, &QWindow::screenChanged, this, [this] { updateSceneSize(); });
    connect(this, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
        updateKeepAwake();
        if (m_RestoreWindowedSize && state == Qt::WindowNoState) {
            m_RestoreWindowedSize = false;
            resize(m_WindowedSize);
        }
    });
}

QtMetalSessionShell::~QtMetalSessionShell()
{
    // Runtime retires this asynchronously before destroying the GUI scene.
    Q_ASSERT(renderingStopped());
    m_Surface->removeEventFilter(this);
    m_Surface.reset();
}

QSize QtMetalSessionShell::defaultWindowedSize(QSize available, QSize desktop)
{
    if (available.isEmpty()) return {};
    const QSize bounds(qMax(1, qRound(available.width() * .8)), qMax(1, qRound(available.height() * .8)));
    return desktop.isEmpty() ? bounds : desktop.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

void QtMetalSessionShell::prepareForSession(QSize desktop, bool fullscreen)
{
    Q_ASSERT(!isVisible() && !m_Rendering->thread);
    if (auto* target = screen()) {
        const auto available = target->availableGeometry();
        resize(defaultWindowedSize(available.size(), desktop));
        setPosition(available.center() - QPoint(width() / 2, height() / 2));
    }
    m_WindowedSize = size();
    create();
    auto& r = *m_Rendering;
    NSView* view = (NSView*)winId();
    r.layer = [metalLayer(view.layer) retain];
    if (!r.layer) {
        emit renderingFailed(QStringLiteral("Qt did not create a native Metal window"));
        return;
    }
    r.device = MTLCreateSystemDefaultDevice();
    r.queue = [r.device newCommandQueue];
    if (!r.device || !r.queue) {
        emit renderingFailed(QStringLiteral("A Metal device is not available"));
        return;
    }
    r.layer.device = r.device;
    r.layer.pixelFormat = MTLPixelFormatRGB10A2Unorm;
    r.layer.opaque = YES;
    r.layer.framebufferOnly = YES;
    r.layer.maximumDrawableCount = 3;
    r.layer.allowsNextDrawableTimeout = YES;
    r.layer.displaySyncEnabled = YES;
    r.layer.wantsExtendedDynamicRangeContent = NO;
    updateColorSpace();
    m_Surface->setGraphicsDevice(QQuickGraphicsDevice::fromDeviceAndCommandQueue(
        (MTLDevice*)r.device, (MTLCommandQueue*)r.queue));
    updateSceneSize();
    r.thread = QThread::create([state = &r] { state->run(); });
    r.thread->setObjectName(QStringLiteral("Qt Metal Render"));
    r.control.prepareThread(r.thread);
    connect(r.thread, &QThread::finished, this, &QtMetalSessionShell::renderingFinished);
    r.thread->start();
    setWindowState(fullscreen ? Qt::WindowFullScreen : Qt::WindowNoState);
}

void QtMetalSessionShell::showSession(bool fullscreen)
{
    if (fullscreen) {
        if (windowState() == Qt::WindowNoState) m_WindowedSize = size();
        m_RestoreWindowedSize = false;
        showFullScreen();
    } else {
        if (windowState() == Qt::WindowFullScreen) m_RestoreWindowedSize = !m_WindowedSize.isEmpty();
        showNormal();
    }
}

void QtMetalSessionShell::stopRendering()
{
    setKeepAwake(false);
    std::lock_guard<std::mutex> lock(m_Rendering->mutex);
    m_Rendering->stop = true;
    m_Rendering->changed.notify_all();
}

bool QtMetalSessionShell::renderingStopped() const
{
    return !m_Rendering->thread || m_Rendering->thread->isFinished();
}

bool QtMetalSessionShell::ownsKeyboard() const
{
    if (!isVisible() || !handle()) return false;
    NSWindow* native = ((NSView*)winId()).window;
    return native && NSApp.isActive && NSApp.keyWindow == native && native.isOnActiveSpace;
}

void QtMetalSessionShell::setKeepAwake(bool enabled)
{
    m_KeepAwake = enabled;
    updateKeepAwake();
}

void QtMetalSessionShell::updateKeepAwake()
{
    auto& assertion = m_Rendering->idleAssertion;
    const bool awake = m_KeepAwake && isVisible() && windowState() != Qt::WindowMinimized;
    if (awake && assertion == kIOPMNullAssertionID) {
        const auto result = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
            kIOPMAssertionLevelOn, CFSTR("PLANK remote desktop session"), &assertion);
        if (result != kIOReturnSuccess) {
            assertion = kIOPMNullAssertionID;
            qWarning("Could not keep the Client display awake (%d)", int(result));
        }
    } else if (!awake && assertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(assertion);
        assertion = kIOPMNullAssertionID;
    }
}

void QtMetalSessionShell::requestRender()
{
    m_Dirty = true;
    if (m_Queued || m_RenderingFrame) return;
    m_Queued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_Queued = false;
        if (!m_Dirty || m_RenderingFrame || !isExposed() || !isVisible() || !m_Rendering->thread) return;
        auto& r = *m_Rendering;
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.stop || r.thread->isFinished()) return;
        r.pixels = size() * devicePixelRatio();
        r.dpr = devicePixelRatio();
        r.layer.drawableSize = CGSizeMake(r.pixels.width(), r.pixels.height());
        m_Dirty = false; m_RenderingFrame = true; r.requested = true;
        r.changed.notify_all();
    }, Qt::QueuedConnection);
}

void QtMetalSessionShell::synchronizeFrame()
{
    auto& r = *m_Rendering;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.stop) return; // A queued sync may outlive the worker's retirement.
    }
    if (!isVisible() || !isExposed() || size() * devicePixelRatio() != r.pixels) {
        std::lock_guard<std::mutex> lock(r.mutex);
        r.cancel = true; r.changed.notify_all(); return;
    }
    r.control.polishItems();
    std::unique_lock<std::mutex> lock(r.mutex);
    if (r.stop) return;
    r.sync = true; r.changed.notify_all();
    // Only scene synchronization holds the GUI. Drawable acquisition, shader
    // compilation/rendering and GPU completion belong to the render worker.
    r.changed.wait(lock, [&r] { return r.synced || r.stop; });
}

void QtMetalSessionShell::frameFinished()
{
    m_RenderingFrame = false;
    if (m_Dirty) requestRender();
}

void QtMetalSessionShell::updateSceneSize()
{
    m_Surface->setScreen(screen());
    m_Surface->setGeometry(0, 0, width(), height());
    requestRender();
}

void QtMetalSessionShell::updateColorSpace()
{
    if (!m_Rendering->layer) return;
    const auto transfer = property("plankMetalTransfer").toString();
    auto space = CGColorSpaceCreateWithName(transfer == QStringLiteral("bt2020") ? kCGColorSpaceITUR_2020 :
        transfer == QStringLiteral("bt709") ? kCGColorSpaceITUR_709 : kCGColorSpaceSRGB);
    // Qt propagates NSWindow's color space to its layer on backing/display
    // changes. Set both so resizing or crossing displays preserves the stream's
    // transfer function instead of replacing it with the monitor profile.
    ((NSView*)winId()).window.colorSpace = [[[NSColorSpace alloc] initWithCGColorSpace:space] autorelease];
    m_Rendering->layer.colorspace = space;
    CGColorSpaceRelease(space);
}

bool QtMetalSessionShell::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Show:
        updateKeepAwake();
        break;
    case QEvent::DynamicPropertyChange:
        if (static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "plankMetalTransfer") updateColorSpace();
        break;
    case QEvent::MouseMove: case QEvent::MouseButtonPress: case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: case QEvent::Wheel: case QEvent::TabletMove:
    case QEvent::TabletPress: case QEvent::TabletRelease: case QEvent::KeyPress:
    case QEvent::KeyRelease: case QEvent::ShortcutOverride: case QEvent::Enter:
    case QEvent::Leave: case QEvent::FocusIn: case QEvent::FocusOut:
        return QCoreApplication::sendEvent(m_Surface.get(), event);
    case QEvent::WindowDeactivate: case QEvent::Hide:
        m_Surface->cancelInput();
        if (event->type() == QEvent::Hide) updateKeepAwake();
        break;
    case QEvent::DevicePixelRatioChange:
        updateSceneSize();
        QCoreApplication::sendEvent(m_Surface.get(), event);
        break;
    default: break;
    }
    return QWindow::event(event);
}

bool QtMetalSessionShell::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_Surface.get() && event->type() == QEvent::CursorChange) setCursor(m_Surface->cursor());
    return QWindow::eventFilter(object, event);
}

void QtMetalSessionShell::resizeEvent(QResizeEvent* event)
{
    QWindow::resizeEvent(event);
    updateSceneSize();
}
void QtMetalSessionShell::exposeEvent(QExposeEvent* event)
{
    QWindow::exposeEvent(event);
    requestRender();
}
void QtMetalSessionShell::closeEvent(QCloseEvent* event)
{
    m_Surface->disconnectSession();
    QWindow::closeEvent(event);
}
