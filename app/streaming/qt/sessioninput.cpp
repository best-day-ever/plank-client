#include "sessioninput.h"
#include "sessionshortcut.h"
#ifdef Q_OS_MACOS
#include "../input/macrawwacom.h"
#include "../mackeyboardcapture.h"
#include "../macquitshortcut.h"
#include "../input/sdl-darwin-scancodes.h"
#else
#include "../input/linuxwacom.h"
#include "../input/linuxrawwacom.h"
#endif
#include <Limelight.h>
#include <plank_transport_pointer.h>
#include <QClipboard>
#include <QGuiApplication>
#include <QThread>
#include <algorithm>
#include <climits>
#include <cmath>
#include <chrono>
#include <memory>
#include <utility>

QtSessionInput::QtSessionInput(QObject* parent) : QObject(parent)
{
    m_PointerDeadline.setSingleShot(true);
    m_PointerDeadline.setInterval(2000);
    connect(&m_PointerDeadline, &QTimer::timeout, this, &QtSessionInput::pointerFailure);
#ifdef Q_OS_MACOS
    const auto ownsKeyboard = [this] {
        return m_Enabled && m_SystemKeys && (m_NativeFocus ? m_NativeFocus() : m_Focused);
    };
    m_MacKeys = std::make_unique<MacKeyboardCapture>(ownsKeyboard, [this] { releaseAll(); });
    // Preserve ordinary menu/Dock Quit, but never let a remotely forwarded
    // Command-Q also activate the local application's Quit shortcut.
    m_MacQuit = std::make_unique<MacQuitShortcut>(ownsKeyboard);
    m_MacKeys->setQueueWake([this] {
        return QMetaObject::invokeMethod(this, [this] {
            m_MacKeys->dispatchPending([this](SDL_KeyboardEvent* event) {
                Qt::KeyboardModifiers mods;
                if (event->mod & SDL_KMOD_CTRL) mods |= Qt::ControlModifier;
                if (event->mod & SDL_KMOD_SHIFT) mods |= Qt::ShiftModifier;
                if (event->mod & SDL_KMOD_ALT) mods |= Qt::AltModifier;
                if (event->mod & SDL_KMOD_GUI) mods |= Qt::MetaModifier;
                for (quint32 code = 0; code < SDL_arraysize(darwin_scancode_table); ++code) {
                    if (event->scancode != SDL_SCANCODE_UNKNOWN && darwin_scancode_table[code] == event->scancode) {
                        key(code, int(qtRemoteKey(code).vk), event->down, event->repeat, mods);
                        break;
                    }
                }
            });
        }, Qt::QueuedConnection);
    });
#endif
}
QtSessionInput::~QtSessionInput()
{
    Q_ASSERT(!m_TabletThread.joinable());
    cancelPointerCapture();
#ifdef Q_OS_MACOS
    m_Enabled = false;
    m_MacQuit.reset();
    m_MacKeys.reset(); // Release callbacks still have valid input state.
#endif
}

void QtSessionInput::startTablets(unsigned hostFeatures)
{
    m_TabletThread = std::thread([this, hostFeatures] { tabletMain(hostFeatures); });
}

void QtSessionInput::tabletMain(unsigned hostFeatures)
{
    const auto activity = [this] { tabletActivity(); };
    std::unique_ptr<QtRawWacomInput> raw;
#ifdef Q_OS_MACOS
    const auto rawFlags = LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND;
    if ((hostFeatures & rawFlags) == rawFlags) raw = std::make_unique<QtRawWacomInput>(activity,
        [this] { return beforeTabletPointerInput(); }, [mutex = m_PointerWireMutex] { mutex->unlock(); });
    m_NormalizedPen.store(!raw && (hostFeatures & LI_FF_PEN_TOUCH_EVENTS));
#else
    std::unique_ptr<LinuxWacomInput> normalized;
    if (!qEnvironmentVariableIntValue("PLANK_EXTERNAL_WACOM_BRIDGE")) {
        const auto decision = plankWacomTransportForConnectedDevice();
        const auto rawFlags = LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND;
        if ((hostFeatures & rawFlags) == rawFlags && decision.transport == PlankWacomTransport::ExactRawHid)
            raw = std::make_unique<LinuxRawWacomInput>(activity);
        else if (hostFeatures & LI_FF_PEN_TOUCH_EVENTS)
            normalized = std::make_unique<LinuxWacomInput>(activity);
    }
#endif
    m_RawControl.publish(raw.get());
    TabletState previous = TabletState::Streaming;
    for (;;) {
        TabletState state;
        bool focused;
        std::uint64_t revision;
        {
            std::unique_lock<std::mutex> lock(m_TabletMutex);
            m_TabletChanged.wait(lock, [this] { return m_RequestedRevision != m_AppliedRevision; });
            state = m_TabletState; focused = m_TabletFocus; revision = m_RequestedRevision;
        }
        if (state != previous) {
            if (state == TabletState::Reconnecting) {
                if (raw) raw->beginReconnect();
#ifdef Q_OS_LINUX
                if (normalized) normalized->beginReconnect();
#endif
            } else if (state == TabletState::Streaming) {
                if (raw) raw->finishReconnect();
#ifdef Q_OS_LINUX
                if (normalized) normalized->finishReconnect();
#endif
            }
            previous = state;
        }
        if (raw) raw->setActive(focused && state == TabletState::Streaming);
#ifdef Q_OS_LINUX
        if (normalized) normalized->setActive(focused && state == TabletState::Streaming);
#endif
        if (state == TabletState::Stopped) break;
        {
            std::lock_guard<std::mutex> lock(m_TabletMutex);
            m_AppliedRevision = revision;
        }
        m_TabletChanged.notify_all();
    }
    m_RawControl.clear();
    raw.reset();
#ifdef Q_OS_LINUX
    normalized.reset();
#else
    m_NormalizedPen.store(false);
#endif
    {
        std::lock_guard<std::mutex> lock(m_TabletMutex);
        m_TabletFinished = true;
    }
    m_TabletChanged.notify_all();
}

void QtSessionInput::transition(TabletState state, bool wait)
{
    Q_ASSERT(!wait || QThread::currentThread() != thread());
    std::unique_lock<std::mutex> lock(m_TabletMutex);
    m_TabletState = state;
    const auto revision = ++m_RequestedRevision;
    m_TabletChanged.notify_all();
    if (wait && m_TabletThread.joinable())
        m_TabletChanged.wait(lock, [this, revision] { return m_AppliedRevision >= revision || m_TabletFinished; });
}
void QtSessionInput::beginReconnect() { transition(TabletState::Reconnecting, true); }
void QtSessionInput::finishReconnect() { transition(TabletState::Streaming, true); }
void QtSessionInput::shutdown()
{
    transition(TabletState::Stopped, true);
    if (m_TabletThread.joinable()) m_TabletThread.join();
}
void QtSessionInput::requestFocus()
{
    std::lock_guard<std::mutex> lock(m_TabletMutex);
    m_TabletFocus = m_Focused && m_Enabled;
    ++m_RequestedRevision;
    m_TabletChanged.notify_all();
}
void QtSessionInput::handleRawHidControl(const unsigned char* data, unsigned int length)
{
    m_RawControl.invoke([&](QtRawWacomInput& input) { input.handleControl(data, length); });
}
void QtSessionInput::handleCursorShape(const unsigned char* data, unsigned int length)
{
    if (m_Cursor.shape(data, length)) QMetaObject::invokeMethod(this, &QtSessionInput::consumeShape, Qt::QueuedConnection);
}
void QtSessionInput::handleCursorPosition(const unsigned char* data, unsigned int length)
{
    const auto size = m_StreamSize.load();
    if (m_Cursor.position(data, length, QSize(int(size >> 32), int(size & 0xffffffffu))))
        QMetaObject::invokeMethod(this, &QtSessionInput::consumePosition, Qt::QueuedConnection);
}
void QtSessionInput::resetCursorEpoch()
{
    m_Cursor.reset();
    m_ActivityPending.store(false);
    QMetaObject::invokeMethod(this, [this] {
        cancelPointerCapture();
        m_LastPositionSize = 0;
        activateMouse();
        emit cursorShape({}, {}, false);
    }, Qt::QueuedConnection);
}
void QtSessionInput::consumeShape()
{
    const auto shape = m_Cursor.takeShape();
    if (!shape) return;
    const QImage image(shape->pixels.data(), int(shape->width), int(shape->height), int(shape->width * 4),
                       QImage::Format_ARGB32_Premultiplied);
    emit cursorShape(image.copy(), QPoint(int(shape->hotspotX), int(shape->hotspotY)), bool(shape->flags & PLANK_CURSOR_FLAG_VISIBLE));
}
void QtSessionInput::consumePosition()
{
    const auto position = m_Cursor.takePosition();
    if (position) emit cursorPosition(QPointF(position->x, position->y));
}
void QtSessionInput::tabletActivity()
{
    if (m_ActivityPending.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] {
        if (!m_ActivityPending.exchange(false) || !m_Enabled || !m_Focused || m_TabletActive) return;
        if (m_PointerState != PointerState::Idle) cancelPointerCapture();
        m_TabletActive = true;
        emit tabletActive(true);
    }, Qt::QueuedConnection);
}
void QtSessionInput::activateMouse()
{
    const bool pendingTabletActivity = m_ActivityPending.exchange(false);
    if (pendingTabletActivity || m_TabletActive) m_LastPositionSize = 0;
    if (!m_TabletActive) return;
    m_TabletActive = false;
    emit tabletActive(false);
}
void QtSessionInput::setStreamSize(QSize size)
{
    const auto previous = m_StreamSize.load();
    m_StreamSize.store((std::uint64_t(size.width()) << 32) | std::uint32_t(size.height()));
    if (previous && previous != m_StreamSize.load())
        QMetaObject::invokeMethod(this, [this] { releaseAll(); }, Qt::QueuedConnection);
}
void QtSessionInput::setEnabled(bool enabled)
{
    if (m_Enabled == enabled) return;
    if (!enabled) { releaseAll(); activateMouse(); }
    m_Enabled = enabled;
    requestFocus();
}
void QtSessionInput::setFocused(bool focused)
{
    if (m_Focused == focused) return;
    if (!focused) { releaseAll(); activateMouse(); }
    m_Focused = focused;
    requestFocus();
}
void QtSessionInput::setSystemKeys(bool captured)
{
    m_SystemKeys = captured;
#ifdef Q_OS_MACOS
    m_MacKeys->refresh();
    m_MacQuit->refresh();
#endif
}
bool QtSessionInput::sendPosition(QPointF position, bool onlyIfChanged)
{
    const auto size = m_StreamSize.load();
    const int width = int(size >> 32), height = int(size & 0xffffffffu);
    if (width <= 0 || height <= 0 || width > SHRT_MAX || height > SHRT_MAX) {
        m_LastPositionSize = 0;
        return false;
    }
    const QPoint pixels(qBound(0, qRound(position.x()), width - 1),
                        qBound(0, qRound(position.y()), height - 1));
    if (onlyIfChanged && m_LastPositionSize == size && m_LastPosition == pixels) return true;
    m_LastPositionSize = 0;
    if (LiSendMousePositionEvent(short(pixels.x()), short(pixels.y()), short(width), short(height)) != 0)
        return false;
    m_LastPosition = pixels;
    m_LastPositionSize = size;
    return true;
}
void QtSessionInput::mouseMotion(QPointF position)
{
    activateMouse();
    if (m_PointerState != PointerState::Idle) return;
    if (m_Enabled) sendPosition(position);
}
void QtSessionInput::mouseButton(int button, bool down, QPointF position)
{
    if (m_PointerState == PointerState::Ending) {
        deferPointer({DeferredPointer::Button, {}, button, down});
        return;
    }
    int wire;
    switch (button) {
    case Qt::LeftButton: wire = BUTTON_LEFT; break;
    case Qt::MiddleButton: wire = BUTTON_MIDDLE; break;
    case Qt::RightButton: wire = BUTTON_RIGHT; break;
    case Qt::BackButton: wire = BUTTON_X1; break;
    case Qt::ForwardButton: wire = BUTTON_X2; break;
    default: return;
    }
    activateMouse();
    if (down) {
        if (!m_Enabled) return;
        if (m_PointerState == PointerState::Idle) {
            if (!sendPosition(position)) return;
        }
        m_Buttons.insert(wire);
    } else if (!m_Buttons.remove(wire)) return;
    LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, wire);
}

void QtSessionInput::setImmersiveMouse(bool enabled, QPointF anchor)
{
    if (enabled == m_ImmersiveMouse) return;
    if (enabled && (!m_PointerCaptureSupported || !m_Enabled || !m_Focused || m_TabletActive)) return;
    if (enabled && !m_Buttons.isEmpty()) {
        emit immersiveMouseChanged(false);
        emit pointerCaptureNotice(tr("Release the mouse buttons before enabling immersive mouse mode."));
        return;
    }
    m_ImmersiveMouse = enabled; emit immersiveMouseChanged(enabled);
    if (enabled) {
        if (m_PointerState == PointerState::Idle) beginPointerCapture(anchor);
    } else if (m_PointerState == PointerState::Captured) {
        for (auto button : std::as_const(m_Buttons)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
        m_Buttons.clear();
        if (!sendPointerCapture(PLANK_POINTER_END)) return;
        m_PointerState = PointerState::Ending; m_PointerDeadline.start();
    }
}

bool QtSessionInput::sendPointerCapture(unsigned char operation, QPointF value)
{
    PlankPointerPacket packet{operation, m_PointerEpoch, ++m_PointerSequence,
        static_cast<uint16_t>(m_PointerSize.width()), static_cast<uint16_t>(m_PointerSize.height()),
        float(value.x()), float(value.y())};
    unsigned char bytes[PLANK_POINTER_PACKET_SIZE];
    plank_pointer_encode(bytes, &packet);
    if (LiSendPlankPointerCapture(bytes, sizeof(bytes)) == 0) return true;
    pointerFailure();
    return false;
}

bool QtSessionInput::beginPointerCapture(QPointF anchor)
{
    std::unique_lock<std::mutex> wireLock(*m_PointerWireMutex, std::try_to_lock);
    if (!wireLock.owns_lock()) {
        m_ImmersiveMouse = false; emit immersiveMouseChanged(false);
        emit pointerCaptureNotice(tr("Tablet input is active. Try immersive mouse mode again after moving the mouse."));
        return false;
    }
    const auto size = m_StreamSize.load();
    m_PointerSize = QSize(int(size >> 32), int(size & 0xffffffffu));
    if (!m_Focused || m_PointerSize.isEmpty() || m_PointerSize.width() > SHRT_MAX ||
        m_PointerSize.height() > SHRT_MAX || !std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) ||
        !m_BeginPointerCapture || !m_BeginPointerCapture()) {
        pointerFailure(); return false;
    }
    ++m_PointerEpoch; m_PointerSequence = 0;
    m_NativePointerCapture = true;
    m_PointerState = PointerState::Captured;
    m_PublishedPointerCapture.store(true);
    m_LastPositionSize = 0;
    emit pointerCaptureChanged(true);
    anchor.setX(qBound(0.0, anchor.x(), double(m_PointerSize.width() - 1)));
    anchor.setY(qBound(0.0, anchor.y(), double(m_PointerSize.height() - 1)));
    return sendPointerCapture(PLANK_POINTER_BEGIN, anchor);
}

void QtSessionInput::mouseDelta(QPointF delta)
{
    if (!std::isfinite(delta.x()) || !std::isfinite(delta.y()) ||
        qAbs(delta.x()) > SHRT_MAX || qAbs(delta.y()) > SHRT_MAX) { pointerFailure(); return; }
    if (m_PointerState == PointerState::Ending) { deferPointer({DeferredPointer::Motion, delta}); return; }
    if (m_PointerState != PointerState::Captured) return;
    if (!delta.isNull()) sendPointerCapture(PLANK_POINTER_MOVE, delta);
}

void QtSessionInput::deferPointer(DeferredPointer event)
{
    if (event.kind == DeferredPointer::Motion && !m_DeferredPointer.isEmpty() &&
        m_DeferredPointer.last().kind == DeferredPointer::Motion) {
        m_DeferredPointer.last().delta += event.delta; return;
    }
    if (m_DeferredPointer.size() >= 128) { pointerFailure(); return; }
    m_DeferredPointer.append(event);
}

void QtSessionInput::handlePointerCapture(const unsigned char* data, unsigned int length)
{
    PlankPointerPacket packet;
    if (plank_pointer_decode(data, length, &packet) || packet.operation != PLANK_POINTER_END_ACK) {
        QMetaObject::invokeMethod(this, &QtSessionInput::pointerFailure, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(this, [this, packet] {
        if (m_PointerState != PointerState::Ending || packet.epoch != m_PointerEpoch ||
            packet.sequence != m_PointerSequence) return;
        if (packet.width != m_PointerSize.width() || packet.height != m_PointerSize.height()) {
            pointerFailure(); return;
        }
        finishPointerCapture({packet.x, packet.y});
    }, Qt::QueuedConnection);
}

void QtSessionInput::finishPointerCapture(QPointF position)
{
    m_PointerDeadline.stop();
    m_PointerState = PointerState::Idle;
    if (m_ImmersiveMouse) beginPointerCapture(position);
    const auto pending = std::exchange(m_DeferredPointer, {});
    for (const auto& event : pending) {
        if (!m_NativePointerCapture || !m_Enabled || !m_Focused) return;
        if (m_PointerState == PointerState::Ending) { deferPointer(event); continue; }
        if (event.kind == DeferredPointer::Motion) {
            if (m_PointerState == PointerState::Captured) mouseDelta(event.delta);
            else {
                position += event.delta;
                position.setX(qBound(0.0, position.x(), double(m_PointerSize.width() - 1)));
                position.setY(qBound(0.0, position.y(), double(m_PointerSize.height() - 1)));
                if (!sendPosition(position)) { pointerFailure(); return; }
            }
        } else if (event.kind == DeferredPointer::Button) mouseButton(event.button, event.down, position);
        else if (event.kind == DeferredPointer::Wheel) wheel(position, event.angle, event.pixel, event.inverted);
        else key(event.scan, event.key, event.down, false, event.modifiers);
    }
    if (m_PointerState == PointerState::Idle && m_NativePointerCapture) {
        m_PublishedPointerCapture.store(false);
        if (m_FinishPointerCapture) m_FinishPointerCapture(position, true);
        m_NativePointerCapture = false;
        emit pointerCaptureChanged(false);
    }
}

void QtSessionInput::cancelPointerCapture()
{
    if (m_ImmersiveMouse) { m_ImmersiveMouse = false; emit immersiveMouseChanged(false); }
    m_DeferredPointer.clear();
    if (m_PointerState == PointerState::Idle) {
        m_PublishedPointerCapture.store(false);
        if (m_NativePointerCapture && m_FinishPointerCapture) m_FinishPointerCapture({}, false);
        if (m_NativePointerCapture) emit pointerCaptureChanged(false);
        m_NativePointerCapture = false;
        return;
    }
    for (auto button : std::as_const(m_Buttons)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    m_Buttons.clear();
    m_PointerDeadline.stop();
    PlankPointerPacket packet{PLANK_POINTER_CANCEL, m_PointerEpoch, ++m_PointerSequence,
        static_cast<uint16_t>(m_PointerSize.width()), static_cast<uint16_t>(m_PointerSize.height()), 0, 0};
    unsigned char bytes[PLANK_POINTER_PACKET_SIZE]; plank_pointer_encode(bytes, &packet);
    LiSendPlankPointerCapture(bytes, sizeof(bytes));
    m_PointerState = PointerState::Idle; m_LastPositionSize = 0;
    m_PublishedPointerCapture.store(false);
    if (m_FinishPointerCapture) m_FinishPointerCapture({}, false);
    m_NativePointerCapture = false;
    emit pointerCaptureChanged(false);
}

void QtSessionInput::pointerFailure()
{
    releaseAll();
    emit pointerCaptureFailed(tr("The remote mouse gesture could not be synchronized. Reconnect to continue."));
}

bool QtSessionInput::beforeTabletPointerInput()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    for (;;) {
        m_PointerWireMutex->lock();
        if (!m_PublishedPointerCapture.load()) return true; // Paired after-report hook unlocks.
        m_PointerWireMutex->unlock();
        struct Barrier { std::mutex mutex; std::condition_variable changed; bool done = false; };
        auto barrier = std::make_shared<Barrier>();
        if (!QMetaObject::invokeMethod(this, [this, barrier] {
            cancelPointerCapture();
            m_TabletActive = true; emit tabletActive(true);
            { std::lock_guard<std::mutex> lock(barrier->mutex); barrier->done = true; }
            barrier->changed.notify_all();
        }, Qt::QueuedConnection)) return false;
        // Never hold the wire mutex while waiting for the GUI. BEGIN uses a
        // nonblocking try-lock; the raw report retains ownership through enqueue.
        std::unique_lock<std::mutex> lock(barrier->mutex);
        if (!barrier->changed.wait_until(lock, deadline, [&] { return barrier->done; })) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
    }
}
void QtSessionInput::wheel(QPointF position, QPoint angleDelta, QPoint pixelDelta, bool inverted)
{
    Q_UNUSED(pixelDelta);
    Q_UNUSED(inverted); // Qt has already applied the compositor's natural-scroll preference.
    if (!m_Enabled || angleDelta.isNull()) return;
    if (m_PointerState == PointerState::Ending) {
        DeferredPointer event{}; event.kind = DeferredPointer::Wheel;
        event.angle = angleDelta; event.pixel = pixelDelta; event.inverted = inverted;
        deferPointer(event); return;
    }
    activateMouse();
    // Preserve entry/reconnect/tablet handoff positioning without injecting
    // an XTEST motion before every wheel event on Linux Hosts. Real motion
    // and button ordering remain unchanged.
    if (m_PointerState == PointerState::Idle && !sendPosition(position, true)) return;
    if (angleDelta.y()) LiSendHighResScrollEvent(short(qBound(int(SHRT_MIN), angleDelta.y(), int(SHRT_MAX))));
    if (angleDelta.x()) LiSendHighResHScrollEvent(short(qBound(int(SHRT_MIN), angleDelta.x(), int(SHRT_MAX))));
}
void QtSessionInput::key(quint32 scan, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers)
{
    if (repeat) return; // repeat is generated on the Host, as in the ordinary Client
    if (!down && m_ShortcutKeys.remove(scan)) return;
    if (down && m_Enabled) {
        const int shortcut = qtSessionShortcut(scan, key, modifiers);
        if (shortcut) {
            m_ShortcutKeys.insert(scan);
            if (shortcut != Qt::Key_M) releaseAll();
            emit shortcutRequested(shortcut); return;
        }
    }
    if (m_PointerState == PointerState::Ending) {
        DeferredPointer event{}; event.kind = DeferredPointer::Key;
        event.scan = scan; event.key = key; event.down = down; event.modifiers = modifiers;
        deferPointer(event); return;
    }
    auto mapped = qtRemoteKey(scan);
    if (down) {
        if (!m_Enabled || !mapped.vk || ((mapped.vk == 0x5b || mapped.vk == 0x5c) && !m_SystemKeys)) return;
        m_Keys.insert(scan, mapped);
    } else {
        auto held = m_Keys.find(scan);
        if (held == m_Keys.end()) return;
        mapped = held.value(); m_Keys.erase(held);
    }
    char flags = 0;
    if (modifiers & Qt::ControlModifier) flags |= MODIFIER_CTRL;
    if (modifiers & Qt::ShiftModifier) flags |= MODIFIER_SHIFT;
    if (modifiers & Qt::AltModifier) flags |= MODIFIER_ALT;
    if (m_SystemKeys && (modifiers & Qt::MetaModifier)) flags |= MODIFIER_META;
    LiSendKeyboardEvent2(short(0x8000 | mapped.vk), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, flags,
                         mapped.nonNormalized ? SS_KBE_FLAG_NON_NORMALIZED : 0);
}
void QtSessionInput::releaseAll()
{
#ifdef Q_OS_MACOS
    m_PenDown = false;
    m_PenPresent = false;
#endif
    m_LastPositionSize = 0;
    for (const auto& key : std::as_const(m_Keys))
        LiSendKeyboardEvent2(short(0x8000 | key.vk), KEY_ACTION_UP, 0, key.nonNormalized ? SS_KBE_FLAG_NON_NORMALIZED : 0);
    m_Keys.clear();
    for (auto button : std::as_const(m_Buttons)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    m_Buttons.clear();
    cancelPointerCapture();
#ifdef Q_OS_MACOS
    if (m_NormalizedPen.load()) LiSendPenEvent(LI_TOUCH_EVENT_CANCEL_ALL, LI_TOOL_TYPE_UNKNOWN, 0,
        0, 0, 0, 0, 0, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
#endif
}

#ifdef Q_OS_MACOS
bool QtSessionInput::capturesNativeKeys() const { return m_MacKeys->suppressSdlKeyEvent(); }

void QtSessionInput::penLeave()
{
    if (!m_PenPresent) return;
    LiSendPenEvent(LI_TOUCH_EVENT_CANCEL_ALL, LI_TOOL_TYPE_UNKNOWN, 0, 0, 0, 0, 0, 0, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
    m_PenDown = false; m_PenPresent = false;
}

void QtSessionInput::normalizedPen(QPointF position, float pressure, float rotation, int xTilt, int yTilt, bool eraser, int buttons)
{
    if (!m_NormalizedPen.load() || !m_Enabled || !m_Focused) return;
    if (m_PointerState != PointerState::Idle) cancelPointerCapture();
    const auto packed = m_StreamSize.load();
    const QSize size(int(packed >> 32), int(packed & 0xffffffffu));
    if (size.isEmpty()) return;
    const bool down = buttons & Qt::LeftButton;
    m_PenPresent = true;
    const auto type = down != m_PenDown ? (down ? LI_TOUCH_EVENT_DOWN : LI_TOUCH_EVENT_UP) :
        (down ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_HOVER);
    m_PenDown = down;
    const unsigned char penButtons = ((buttons & Qt::RightButton) ? LI_PEN_BUTTON_PRIMARY : 0) |
        ((buttons & Qt::MiddleButton) ? LI_PEN_BUTTON_SECONDARY : 0);
    const auto tilt = uint8_t(qBound(0, qRound(std::hypot(xTilt, yTilt)), 90));
    LiSendPenEvent(type, eraser ? LI_TOOL_TYPE_ERASER : LI_TOOL_TYPE_PEN, penButtons,
        float(qBound(0.0, position.x() / size.width(), 1.0)), float(qBound(0.0, position.y() / size.height(), 1.0)),
        qBound(0.0f, pressure, 1.0f), 0, 0, uint16_t((qRound(rotation) + 360) % 360), tilt);
    tabletActivity();
}
#endif
void QtSessionInput::pasteText()
{
    if (!m_Enabled) return;
    releaseAll();
    auto text = QGuiApplication::clipboard()->text().replace(QStringLiteral("\r\n"), QStringLiteral("\n")).toUtf8();
    if (text.size() <= 512 * 1024) LiSendUtf8TextEvent(text.constData(), unsigned(text.size()));
}
