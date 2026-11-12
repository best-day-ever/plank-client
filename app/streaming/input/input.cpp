#include <Limelight.h>
#include <plank.h>
#include <SDL3/SDL.h>
#include "streaming/input/input.h"
#include "streaming/session.h"
#include "streaming/plankwaylandcursor.h"
#include "streaming/streamutils.h"
#include "utils.h"
#ifdef Q_OS_MACOS
#include "streaming/macquitshortcut.h"
#include "streaming/mackeyboardcapture.h"
#include "streaming/macwindow.h"
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include "streaming/input/macpen.h"
#endif
#ifdef Q_OS_WIN
#include "streaming/input/windowsrawwacom.h"
#endif

#ifdef HAVE_MAC_RAW_WACOM
#include "streaming/input/macrawwacom.h"
#endif

#ifdef HAVE_LIBINPUT_TABLET
#include "streaming/input/linuxwacom.h"
#include "streaming/input/linuxrawwacom.h"
#endif

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>
#include <QtEndian>

#include <algorithm>
#include <cstring>

SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs,
                                 int streamWidth,
                                 int streamHeight)
    : m_Window(nullptr),
      m_NeedsManualCaptureOnLeave(false),
      m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false),
      m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false),
      m_LocalToolbarAvailable(false),
      m_LocalCursorSupported(false),
      m_RemoteCursorVisible(true),
      m_CompositorCursorRequestedVisible(true),
      m_TabletCursorActive(false),
      m_FakeMouseCaptureActive(false),
      m_HasActivatedCapture(false),
      m_MousePressFailureLogs(0),
      m_KeyboardCaptureActive(false),
      m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_ImmersiveKeyboardMode(false),
      m_MouseCursorCapturedVisibilityState(false),
      m_StreamDimensions(
          (static_cast<std::uint64_t>(streamWidth) << 32) |
          static_cast<std::uint32_t>(streamHeight))
{
#ifdef Q_OS_MACOS
    m_ImmersiveKeyboardMode = prefs.immersiveKeyboardMode;
    if (m_ImmersiveKeyboardMode) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }
#endif
    // System keys are always captured when running without a DE
    if (!WMUtils::isRunningDesktopEnvironment()) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }

    // Native SDL3 auto-capture is used; the SDL2 leave workaround is obsolete.
    m_NeedsManualCaptureOnLeave = false;

    // Opt-out of SDL's built-in Alt+Tab handling while keyboard grab is enabled
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");

    // Allow clicks to pass through to us when focusing the window. If we're in
    // absolute mouse mode, this will avoid the user having to click twice to
    // trigger a click on the host if the Moonlight window is not focused. In
    // relative mode, the click event will trigger the mouse to be recaptured.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // Populate special key combo configuration
    m_SpecialKeyCombos[KeyComboUngrabInput].keyCombo = KeyComboUngrabInput;
    m_SpecialKeyCombos[KeyComboUngrabInput].keyCode = SDLK_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].scanCode = SDL_SCANCODE_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCombo = KeyComboToggleFullScreen;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCode = SDLK_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].scanCode = SDL_SCANCODE_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCombo = KeyComboToggleStatsOverlay;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCode = SDLK_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].scanCode = SDL_SCANCODE_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCombo = KeyComboToggleMinimize;
    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCode = SDLK_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].scanCode = SDL_SCANCODE_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].enabled = WMUtils::isRunningDesktopEnvironment();

    m_SpecialKeyCombos[KeyComboPasteText].keyCombo = KeyComboPasteText;
    m_SpecialKeyCombos[KeyComboPasteText].keyCode = SDLK_V;
    m_SpecialKeyCombos[KeyComboPasteText].scanCode = SDL_SCANCODE_V;
    m_SpecialKeyCombos[KeyComboPasteText].enabled = true;

    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCombo = KeyComboTogglePointerRegionLock;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCode = SDLK_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].scanCode = SDL_SCANCODE_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCombo = KeyComboToggleKeyboardGrab;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].keyCode = SDLK_K;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].scanCode = SDL_SCANCODE_K;
    m_SpecialKeyCombos[KeyComboToggleKeyboardGrab].enabled =
            WMUtils::isRunningDesktopEnvironment();

#ifdef Q_OS_MACOS
    auto ownsKeyboard = [this] { return isSystemKeyCaptureActive(); };
    m_MacQuitShortcut = std::make_unique<MacQuitShortcut>(ownsKeyboard);
    m_MacKeyboardCapture = std::make_unique<MacKeyboardCapture>(
        ownsKeyboard, [this] { raiseAllKeys(); });
    m_MacKeyboardCapture->setSessionHotkeyHandler([this](const SDL_KeyboardEvent& key) {
        return m_SessionHotkeys.handle(key);
    });
#endif
}

#ifdef Q_OS_MACOS
bool SdlInputHandler::hasMacStreamKeyboardFocus() const
{
    // SDL focus notifications may still be queued. A local Qt dialog must
    // never inherit the stream's shortcut ownership or forward its typing.
    return MacRawWacomFocus::streamHasFocus(
                m_PresentationLayout.outputs, m_Window, MacWindow::hasKeyboardFocus);
}
#endif

void SdlInputHandler::setStreamDimensions(int streamWidth, int streamHeight)
{
    SDL_assert(streamWidth > 0);
    SDL_assert(streamHeight > 0);
    m_StreamDimensions.store(
                (static_cast<std::uint64_t>(streamWidth) << 32) |
                static_cast<std::uint32_t>(streamHeight),
                std::memory_order_relaxed);
}

QSize SdlInputHandler::streamDimensions() const
{
    const std::uint64_t dimensions =
            m_StreamDimensions.load(std::memory_order_relaxed);
    return QSize(static_cast<int>(dimensions >> 32),
                 static_cast<int>(dimensions & 0xffffffffu));
}

SdlInputHandler::~SdlInputHandler()
{
    hideSharedPointer();
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture.reset();
    m_MacQuitShortcut.reset();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    m_MacRawWacomInput.reset();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    m_LinuxWacomInput.reset();
    m_LinuxRawWacomInput.reset();
#endif
#ifdef Q_OS_WIN
    m_WindowsRawWacomInput.reset();
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    m_SdlPenInput.reset();
#endif

    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    m_WaylandTabletCursorOutputs.clear();

    if (m_RemoteCursor != nullptr) {
        SDL_DestroyCursor(m_RemoteCursor);
        m_RemoteCursor = nullptr;
    }

#ifdef STEAM_LINK
    // Hide SDL's cursor on Steam Link after quitting the stream.
    // FIXME: We should also do this for other situations where SDL
    // and Qt will draw their own mouse cursors like KMSDRM or RPi
    // video backends.
    setCursorVisible(false);
#endif
}

void SdlInputHandler::setWindow(SDL_Window *window)
{
    m_Window = window;
    m_LocalCursorSupported =
            (LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR) != 0;
    if (m_LocalCursorSupported) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PLANK local cursor transport enabled");
        setCursorVisible(true);
        ensureWaylandTabletCursorAttached(m_Window);
    }
    else {
        // Session accepts this only for the authenticated embedded-cursor
        // contract. Keep the local pointer for toolbar/letterbox interaction.
        m_MouseCursorCapturedVisibilityState = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "PLANK embedded host cursor enabled");
    }
#if defined(HAVE_LIBINPUT_TABLET) || defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    const auto requestTabletCursor = [this]() {
        if (!m_TabletCursorActivationPending.exchange(true)) {
            Session::postTabletCursorActivationEvent();
        }
    };
#endif
#ifdef HAVE_MAC_RAW_WACOM
    // The SDL pen path is an explicit fallback. Never claim the physical
    // tablet through both transports in the same session.
    if (qEnvironmentVariableIntValue("PLANK_MACOS_PEN_INPUT") == 0 &&
            (LiGetHostFeatureFlags() &
             (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
            (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                    "Using Mac Wacom raw HID forwarding");
        m_MacRawWacomInput.reset(new MacRawWacomInput(requestTabletCursor));
        refreshTabletFocus();
    }
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (qEnvironmentVariableIntValue("PLANK_EXTERNAL_WACOM_BRIDGE") != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                    "Normalized Wacom capture disabled for external raw-HID qualification");
    }
    else if ((LiGetHostFeatureFlags() &
              (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
             (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        const PlankWacomTransportDecision decision =
            plankWacomTransportForConnectedDevice();
        if (decision.transport == PlankWacomTransport::NormalizedPen) {
            if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                            "Using normalized pen transport for first-generation Intuos Pro %04x:%04x",
                            static_cast<unsigned int>(decision.vendor),
                            static_cast<unsigned int>(decision.product));
                m_LinuxWacomInput.reset(new LinuxWacomInput(requestTabletCursor));
                m_LinuxWacomInput->setActive(
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
            }
            else {
                SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                             "Host lacks normalized pen support required by first-generation Intuos Pro %04x:%04x",
                             static_cast<unsigned int>(decision.vendor),
                             static_cast<unsigned int>(decision.product));
            }
        }
        else {
            m_LinuxRawWacomInput.reset(new LinuxRawWacomInput(requestTabletCursor));
            m_LinuxRawWacomInput->setActive(
                (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
        }
    }
    else if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
        m_LinuxWacomInput.reset(new LinuxWacomInput(requestTabletCursor));
        m_LinuxWacomInput->setActive(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
#endif
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIntValue("PLANK_WINDOWS_PEN_INPUT") == 0 &&
            (LiGetHostFeatureFlags() &
             (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
            (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        m_WindowsRawWacomInput.reset(new WindowsRawWacomInput(requestTabletCursor));
        m_WindowsRawWacomInput->setActive(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    // SDL pen events provide Windows Ink fallback. macOS retains its
    // explicit fallback opt-in; raw and normalized never forward together.
    if (
#ifdef Q_OS_MACOS
            qEnvironmentVariableIntValue("PLANK_MACOS_PEN_INPUT") != 0
#else
            true // Windows Ink fallback remains available if raw USB capture fails.
#endif
            ) {
        if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                        "Using SDL3 pen events for PLANK normalized pen transport");
            m_SdlPenInput.reset(new PlankMacPenInput(
                [](const PlankMacPenPacket& packet) {
                    LiSendPenEvent(packet.eventType, packet.toolType,
                                   packet.buttons, packet.x, packet.y,
                                   packet.pressureOrDistance,
                                   0.0f, 0.0f,
                                   packet.rotation, packet.tilt);
                },
                requestTabletCursor));
            m_SdlPenInput->setActive(
                (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
        }
        else {
            SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                         "Host lacks normalized pen support (LI_FF_PEN_TOUCH_EVENTS); "
                         "normalized pen/tablet input will not be forwarded");
        }
    }
#endif
}

void SdlInputHandler::setPresentationLayout(
        const PlankPresentationLayout& layout)
{
    hideSharedPointer();
    m_PresentationLayout = layout;
    if (m_PresentationLayout.outputs.isEmpty() && m_Window != nullptr) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(m_Window, &width, &height);
        m_PresentationLayout.canvasSize = QSize(qMax(1, width), qMax(1, height));
        m_PresentationLayout.outputs.append(
            {m_Window, QRect(QPoint(0, 0), m_PresentationLayout.canvasSize), true});
    }
    refreshTabletFocus();
    reconcileWaylandTabletCursorOutputs();
    updateTabletCursorVisibility();
    updatePointerRegionLock();
}

void SdlInputHandler::setLiveDrawableGeometry(bool live)
{
    if (m_LiveDrawableGeometry.exchange(live, std::memory_order_relaxed) ==
            live) {
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "PLANK input geometry: renderer letterboxes against the %s",
                live ? "live drawable" : "presentation layout snapshot");
    updatePointerRegionLock();
}

void SdlInputHandler::updateDecodedStreamDimensions(int frameWidth,
                                                    int frameHeight)
{
    if (frameWidth <= 0 || frameHeight <= 0) {
        return;
    }
    const QSize current = streamDimensions();
    if (current == QSize(frameWidth, frameHeight)) {
        return;
    }
    // Only reported for renderers that letterbox the decoded frame size
    // (IFFmpegRenderer::letterboxesDecodedFrameSize()); input must use that
    // size too or pointer and picture disagree in aspect.
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "PLANK input geometry: decoded frames are %dx%d, not the "
                "negotiated %dx%d; mapping input to the decoded size",
                frameWidth, frameHeight, current.width(), current.height());
    setStreamDimensions(frameWidth, frameHeight);
    updatePointerRegionLock();
}

void SdlInputHandler::notifyWindowGeometryChanged(SDL_Window* window,
                                                  const char* reason)
{
    PlankOutputGeometry geometry;
    if (window == nullptr || !outputGeometry(window, geometry)) {
        return;
    }
    const QSize streamSize = streamDimensions();
    const QRect video =
            PlankPresentation::videoRect(streamSize, geometry.canvasSize);
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "PLANK input geometry (%s): window=%dx%d canvas=%dx%d%s "
                "output=%d,%d %dx%d video=%d,%d %dx%d stream=%dx%d",
                reason,
                geometry.windowSize.width(), geometry.windowSize.height(),
                geometry.canvasSize.width(), geometry.canvasSize.height(),
                geometry.live ? " (live drawable)" : " (layout snapshot)",
                geometry.canvasRect.x(), geometry.canvasRect.y(),
                geometry.canvasRect.width(), geometry.canvasRect.height(),
                video.x(), video.y(), video.width(), video.height(),
                streamSize.width(), streamSize.height());
    if (window == m_Window) {
        updatePointerRegionLock();
    }
}

void SdlInputHandler::refreshWaylandTabletCursorParents()
{
    if (!m_LocalCursorSupported) {
        return;
    }

    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    m_WaylandTabletCursorOutputs.clear();

    // SDL_CreateRenderer() may replace a Wayland window's native wl_surface.
    // A replacement proxy can reuse the same client address, so raw pointer
    // identity cannot reliably prove that an existing subsurface still has a
    // live parent. Rebuild from the final SDL windows after renderer setup.
    reconcileWaylandTabletCursorOutputs();
    updateTabletCursorVisibility();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Refreshed PLANK Wacom cursor parents for %zu presentation output%s",
                m_WaylandTabletCursorOutputs.size(),
                m_WaylandTabletCursorOutputs.size() == 1 ? "" : "s");
}

bool SdlInputHandler::handleRemoteCursorChunk(const unsigned char* data, unsigned int length)
{
    return m_RemoteCursorMailbox.shape(data, length);
}

void SdlInputHandler::applyPendingRemoteCursor()
{
    auto pending = m_RemoteCursorMailbox.takeShape();
    if (!pending) return;
    auto cursor = std::move(*pending);

    const bool firstCursor = m_RemoteCursor == nullptr;
    m_AppliedRemoteCursor = cursor;
    m_AppliedRemoteCursorValid = true;
    m_RemoteCursorLogicalSize = {};
    refreshRemoteCursorScale(SDL_GetMouseFocus());
    m_RemoteCursorVisible =
            (cursor.flags & PLANK_CURSOR_FLAG_VISIBLE) != 0;
    reconcileWaylandTabletCursorOutputs();
    const QImage cursorImage(
                m_AppliedRemoteCursor.pixels.data(),
                static_cast<int>(m_AppliedRemoteCursor.width),
                static_cast<int>(m_AppliedRemoteCursor.height),
                static_cast<int>(m_AppliedRemoteCursor.width * 4U),
                QImage::Format_ARGB32_Premultiplied);
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setImage(
                cursorImage,
                static_cast<int>(m_AppliedRemoteCursor.hotspotX),
                static_cast<int>(m_AppliedRemoteCursor.hotspotY));
        output.cursor->dispatchPending();
    }
    updateTabletCursorVisibility();
    if (isCaptureActive()) {
        setCursorVisible(!m_MouseWasInVideoRegion || m_RemoteCursorVisible);
    }

    if (firstCursor) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Applied first PLANK local cursor (%ux%u hotspot %u,%u visible=%d)",
                    cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                    m_RemoteCursorVisible ? 1 : 0);
    }
    else {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "Applied PLANK local cursor generation %llu (%ux%u hotspot %u,%u visible=%d)",
                     static_cast<unsigned long long>(cursor.generation),
                     cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                     m_RemoteCursorVisible ? 1 : 0);
    }
}

void SdlInputHandler::refreshRemoteCursorScale(SDL_Window* window)
{
    if (!m_AppliedRemoteCursorValid) return;
    if (!presentationOutput(window)) window = m_Window;
    PlankOutputGeometry geometry;
    if (!window || !outputGeometry(window, geometry)) return;
    const auto& image = m_AppliedRemoteCursor;
    double sx = 1, sy = 1;
#ifdef Q_OS_MACOS
    const auto* output = presentationOutput(window);
    if (output && output->sourceRect.isValid()) {
        int dw = 0, dh = 0;
        SDL_GetWindowSizeInPixels(window, &dw, &dh);
        const auto fit = PlankPresentation::aspectFitRect(output->sourceRect.size().toSize(), QRect(0, 0, dw, dh));
        if (dw <= 0 || dh <= 0) return;
        sx = fit.width() / output->sourceRect.width() * geometry.windowSize.width() / dw;
        sy = fit.height() / output->sourceRect.height() * geometry.windowSize.height() / dh;
    } else {
        const QSize stream = streamDimensions();
        const QRect video = PlankPresentation::videoRect(stream, geometry.canvasSize);
        if (stream.isEmpty() || video.isEmpty()) return;
        sx = double(video.width()) / stream.width() * geometry.windowSize.width() / geometry.canvasRect.width();
        sy = double(video.height()) / stream.height() * geometry.windowSize.height() / geometry.canvasRect.height();
    }
#endif
    const QSize logicalSize(qBound(1, qRound(image.width * sx), 512), qBound(1, qRound(image.height * sy), 512));
    if (m_RemoteCursor && logicalSize == m_RemoteCursorLogicalSize) return;
    SDL_Surface* original = SDL_CreateSurfaceFrom(int(image.width), int(image.height), SDL_PIXELFORMAT_ARGB8888,
        const_cast<unsigned char*>(image.pixels.data()), int(image.width * 4));
    if (!original) return;
    SDL_Surface* base = SDL_ScaleSurface(original, logicalSize.width(), logicalSize.height(), SDL_SCALEMODE_LINEAR);
    if (!base) { SDL_DestroySurface(original); return; }
    // Cocoa consumes the base size as logical points and the original as a
    // high-density representation. Preserve sharp cursor pixels on Retina.
    if (logicalSize != QSize(int(image.width), int(image.height))) SDL_AddSurfaceAlternateImage(base, original);
    const int hx = qBound(0, qRound(image.hotspotX * sx), logicalSize.width() - 1);
    const int hy = qBound(0, qRound(image.hotspotY * sy), logicalSize.height() - 1);
    SDL_Cursor* replacement = SDL_CreateColorCursor(base, hx, hy);
    SDL_DestroySurface(base); SDL_DestroySurface(original);
    if (!replacement) return;
    SDL_SetCursor(replacement);
    if (m_RemoteCursor) SDL_DestroyCursor(m_RemoteCursor);
    m_RemoteCursor = replacement; m_RemoteCursorLogicalSize = logicalSize;
}

bool SdlInputHandler::handleRemoteCursorPosition(const unsigned char* data, unsigned int length)
{
    return m_RemoteCursorMailbox.position(data, length, streamDimensions());
}

void SdlInputHandler::applyPendingRemoteCursorPosition()
{
    const auto pending = m_RemoteCursorMailbox.takePosition();
    if (!pending) return;
    const auto position = *pending;

    m_AppliedRemoteCursorPosition = position;
    m_AppliedRemoteCursorPositionValid = true;
    m_AppliedRemoteCursorPositionSequence = position.sequence;
    updateSharedPointer();
    SDL_Window* targetWindow = nullptr;
    int x = 0;
    int y = 0;
    if (!mapRemoteCursorPositionToWindow(position, targetWindow, x, y)) {
        return;
    }
#ifdef HAVE_MAC_RAW_WACOM
    updateTabletCursorVisibility();
    if (m_MacRawWacomInput) {
        followPointerFocus(targetWindow, 0, PointerFocusPosition::HostTablet);
    }
#endif
    PlankWaylandCursor* cursor =
            ensureWaylandTabletCursorAttached(targetWindow);
    if (cursor == nullptr) {
        return;
    }
    cursor->setPosition(x, y);
    cursor->dispatchPending();
    updateTabletCursorVisibility();
}

void SdlInputHandler::applyPendingTabletCursorActivation()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    refreshNormalizedPen();
#endif
    // A mouse or toolbar event can cancel tablet ownership after this SDL event
    // was queued. Never let that stale event steal ownership back.
    if (!m_TabletCursorActivationPending.load()) {
        return;
    }
    if (!m_LocalCursorSupported) {
        m_TabletCursorActivationPending.store(false);
        if (isCaptureActive()) setCursorVisible(false);
        return;
    }
    reconcileWaylandTabletCursorOutputs();
    if (!m_LocalCursorSupported || !isCaptureActive() ||
            (m_WaylandTabletCursorOutputs.empty()
#ifdef HAVE_MAC_RAW_WACOM
             && !m_MacRawWacomInput
#endif
             )) {
        m_TabletCursorActivationPending.store(false);
        return;
    }

    if (!m_TabletCursorActive) {
        m_TabletCursorActive = true;
        m_TabletCursorActivationSequence =
                m_AppliedRemoteCursorPositionSequence;
        SDL_HideCursor();
        updateTabletCursorVisibility();
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "Switched to host-authoritative Wacom cursor position");
    }
}

void SdlInputHandler::raiseAllKeys()
{
    if (m_KeysDown.isEmpty()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Raising %d keys",
                (int)m_KeysDown.count());

    for (auto keyDown : std::as_const(m_KeysDown)) {
        LiSendKeyboardEvent(keyDown, KEY_ACTION_UP, 0);
    }

    m_KeysDown.clear();
    m_FilePasteKeyUpConsumed = false;
}

void SdlInputHandler::notifyMouseLeave()
{
    // SDL on Windows doesn't send the mouse button up until the mouse re-enters the window
    // after leaving it. This breaks some of the Aero snap gestures, so we'll capture it to
    // allow us to receive the mouse button up events later.
    //
    // On macOS and X11, capturing the mouse allows us to receive mouse motion outside the
    // window (button up already worked without capture).
    if (m_NeedsManualCaptureOnLeave && isCaptureActive()) {
        // NB: Not using SDL_GetGlobalMouseState() because we want our state not the system's
        Uint32 mouseState = SDL_GetMouseState(nullptr, nullptr);
        for (Uint32 button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; button++) {
            if (mouseState & SDL_BUTTON_MASK(button)) {
                SDL_CaptureMouse(true);
                break;
            }
        }
    }
}

void SdlInputHandler::notifyFocusLost()
{
    hideSharedPointer();
    m_SessionHotkeys.releaseKeys();
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
    activateCompositorCursor();
#ifdef HAVE_MAC_RAW_WACOM
    refreshTabletFocus();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(false);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(false);
    }
#endif
#ifdef Q_OS_WIN
    refreshTabletFocus();
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (m_SdlPenInput) {
        m_SdlPenInput->setActive(false);
    }
#endif

    // Raise all keys that are currently pressed. If we don't do this, certain keys
    // used in shortcuts that cause focus loss (such as Alt+Tab) may get stuck down.
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    refreshTabletFocus();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(true);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(true);
    }
#endif
#ifdef Q_OS_WIN
    refreshTabletFocus();
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    refreshNormalizedPen();
#endif
}

void SdlInputHandler::refreshTabletFocus()
{
#ifdef Q_OS_WIN
    bool focused = m_Window && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS);
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window && (SDL_GetWindowFlags(output.window) & SDL_WINDOW_INPUT_FOCUS)) focused = true;
    }
    if (m_WindowsRawWacomInput) m_WindowsRawWacomInput->setActive(isCaptureActive() && focused);
    refreshNormalizedPen();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    if (!m_MacRawWacomInput) return;

    // Native fullscreen Spaces can change the key window before SDL updates
    // its cached focus flags, or without a matching SDL focus event. Keep the
    // raw tablet lease while either presentation window owns native focus.
    const bool captureActive = isCaptureActive();
    const bool nativeFocus = hasMacStreamKeyboardFocus();
    const auto active = m_MacRawWacomFocus.update(captureActive, nativeFocus);
    if (!active) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "Mac Wacom stream focus: active=%d capture=%d native=%d",
                *active, captureActive, nativeFocus);
    m_MacRawWacomInput->setActive(*active);
#endif
}

void SdlInputHandler::handleRawHidControl(const unsigned char* data,
                                          unsigned int length)
{
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->handleControl(data, length);
#elif defined(Q_OS_WIN)
    if (m_WindowsRawWacomInput) m_WindowsRawWacomInput->handleControl(data, length);
#elif defined(HAVE_LIBINPUT_TABLET)
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->handleControl(data, length);
    }
#else
    Q_UNUSED(data);
    Q_UNUSED(length);
#endif
}

void SdlInputHandler::beginRawHidReconnect()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    if (m_SdlPenInput) m_SdlPenInput->setActive(false);
#endif
#ifdef Q_OS_WIN
    if (m_WindowsRawWacomInput) m_WindowsRawWacomInput->beginReconnect();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->beginReconnect();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->beginReconnect();
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->beginReconnect();
    }
#endif
}

void SdlInputHandler::finishRawHidReconnect()
{
#ifdef Q_OS_WIN
    if (m_WindowsRawWacomInput) m_WindowsRawWacomInput->finishReconnect();
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    refreshNormalizedPen();
#endif
#ifdef HAVE_MAC_RAW_WACOM
    if (m_MacRawWacomInput) m_MacRawWacomInput->finishReconnect();
#endif
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->finishReconnect();
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->finishReconnect();
    }
#endif
}

void SdlInputHandler::resetRemoteCursorPositionEpoch()
{
    // The login-screen and authenticated-desktop media workers each start
    // their cursor position sequence at one. The input handler survives that
    // handoff, so discard the old worker's high-water mark once its transport
    // has stopped or the new worker's positions will be rejected until its
    // counter catches up.
    hideSharedPointer();
    m_RemoteCursorMailbox.resetPositionEpoch();

    m_AppliedRemoteCursorPosition = {};
    m_AppliedRemoteCursorPositionValid = false;
    m_AppliedRemoteCursorPositionSequence = 0;
    m_TabletCursorActivationSequence = 0;
    m_TabletCursorActivationPending.store(false);
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "Reset PLANK remote cursor position epoch for desktop reconnect");
}

bool SdlInputHandler::isCaptureActive()
{
    return m_FakeMouseCaptureActive;
}

void SdlInputHandler::setToolbarInteractionActive(bool active)
{
    if (active) {
        activateCompositorCursor();
    }
    setCursorVisible(active ? true :
                     (!m_MouseWasInVideoRegion ||
                      (m_LocalCursorSupported ? m_RemoteCursorVisible :
                                                m_MouseCursorCapturedVisibilityState)));
}

void SdlInputHandler::setLocalToolbarAvailable(bool available)
{
    if (m_LocalToolbarAvailable == available) {
        return;
    }

    m_LocalToolbarAvailable = available;
    updatePointerRegionLock();
}

void SdlInputHandler::updateKeyboardGrabState()
{
    bool shouldGrab = m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF && isCaptureActive();
    if (shouldGrab) {
        Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
        if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
            // Ungrab if it's fullscreen only and we left fullscreen
            shouldGrab = false;
        }
    }

    // Don't close the window on Alt+F4 when keyboard grab is enabled
    SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, shouldGrab ? "0" : "1");

#ifndef Q_OS_MACOS
    for (const auto& output : m_PresentationLayout.outputs) {
        SDL_SetWindowKeyboardGrab(output.window, shouldGrab ? true : false);
    }
#endif

    m_KeyboardCaptureActive = shouldGrab;
#ifdef Q_OS_MACOS
    m_MacKeyboardCapture->refresh();
    m_MacQuitShortcut->refresh();
#endif
}

bool SdlInputHandler::isSystemKeyCaptureActive()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF) {
        return false;
    }

    if (m_Window == nullptr) {
        return false;
    }

    // NB: We used to check SDL_WINDOW_KEYBOARD_GRABBED here, but this isn't
    // always set when capture "fails" on SDL3, even though the user may have
    // configured the compositor to pass through system keys to us anyway.
    // See issues #1776 and #1900 for details.
    bool focused = false;
    bool fullscreen = false;
    for (const auto& output : m_PresentationLayout.outputs) {
        const Uint32 windowFlags = SDL_GetWindowFlags(output.window);
#ifdef Q_OS_MACOS
        // AppKit can return to a fullscreen Space before SDL's cached focus
        // flags recover. Do not require both native and cached focus to agree.
        focused = focused || MacWindow::hasKeyboardFocus(output.window);
#else
        focused = focused || (windowFlags & SDL_WINDOW_INPUT_FOCUS);
#endif
        fullscreen = fullscreen || (windowFlags & SDL_WINDOW_FULLSCREEN);
    }
    if (!focused || !m_KeyboardCaptureActive) {
        return false;
    }

    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !fullscreen) {
        return false;
    }

    return true;
}

void SdlInputHandler::setCaptureActive(bool active)
{
    active = active && !m_ViewOnly;
    if (active) {
        setCursorVisible(m_LocalCursorSupported ?
                             (!m_MouseWasInVideoRegion || m_RemoteCursorVisible) :
                             m_MouseCursorCapturedVisibilityState);
        m_FakeMouseCaptureActive = true;
        m_HasActivatedCapture = true;

        // Synchronize the client and host cursor when activating absolute capture
        float mouseX, mouseY;
        int windowX, windowY;

        // We have to use SDL_GetGlobalMouseState() because macOS may not reflect
        // the new position of the mouse when outside the window.
        SDL_GetGlobalMouseState(&mouseX, &mouseY);

        // Convert global mouse state to window-relative
        SDL_GetWindowPosition(m_Window, &windowX, &windowY);
        mouseX -= windowX;
        mouseY -= windowY;

        if (isMouseInVideoRegion(mouseX, mouseY,
                                 SDL_GetWindowID(m_Window))) {
            // Synthesize a mouse event to synchronize the cursor
            SDL_MouseMotionEvent motionEvent = {};
            motionEvent.type = SDL_EVENT_MOUSE_MOTION;
            motionEvent.timestamp = SDL_GetTicksNS();
            motionEvent.windowID = SDL_GetWindowID(m_Window);
            motionEvent.x = mouseX;
            motionEvent.y = mouseY;
            handleMouseMotionEvent(&motionEvent);
        }
    }
    else {
        activateCompositorCursor();
        setCursorVisible(true);
        m_FakeMouseCaptureActive = false;
    }

    refreshTabletFocus();

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}

void SdlInputHandler::setCursorVisible(bool visible)
{
    m_CompositorCursorRequestedVisible = visible;
    if (!m_LocalCursorSupported && !m_EmbeddedCursor.setVisible(visible)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Unable to update embedded-session cursor image: %s", SDL_GetError());
    }
    if (visible && !m_TabletCursorActive) {
        SDL_ShowCursor();
    }
    else {
        SDL_HideCursor();
    }
    updateTabletCursorVisibility();
}

void SdlInputHandler::activateCompositorCursor()
{
    m_TabletCursorActivationPending.store(false);
    if (!m_TabletCursorActive) {
        return;
    }

    m_TabletCursorActive = false;
#ifdef HAVE_MAC_RAW_WACOM
    for (const auto& output : m_PresentationLayout.outputs)
        MacWindow::hideTabletCursor(output.window);
    MacWindow::hideTabletCursor(m_Window);
#endif
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(false);
        output.cursor->dispatchPending();
    }
    if (m_CompositorCursorRequestedVisible) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                 "Restored Wayland compositor cursor for mouse input");
}

bool SdlInputHandler::mapRemoteCursorPositionToWindow(
        const RemoteCursorPosition& position, SDL_Window*& window,
        int& x, int& y) const
{
    if (m_Window == nullptr || position.frameWidth == 0 ||
            position.frameHeight == 0) {
        return false;
    }

    const QPointF streamPoint(position.x, position.y);
    if (m_PresentationLayout.usesSourceRects()) {
        // The cursor is in capture (stream) pixels: the window whose source
        // rectangle holds it shows it.
        for (const auto& output : m_PresentationLayout.outputs) {
            int windowWidth = 0;
            int windowHeight = 0;
            int drawableWidth = 0;
            int drawableHeight = 0;
            SDL_GetWindowSize(output.window, &windowWidth, &windowHeight);
            SDL_GetWindowSizeInPixels(output.window, &drawableWidth, &drawableHeight);
            QPointF windowPoint;
            if (PlankPresentation::mapStreamPointToSourceWindow(
                        streamPoint, QSize(position.frameWidth, position.frameHeight), output,
                        QSize(windowWidth, windowHeight), QSize(drawableWidth, drawableHeight),
                        windowPoint)) {
                window = output.window;
                x = qRound(windowPoint.x());
                y = qRound(windowPoint.y());
                return true;
            }
        }
        return false;
    }
    for (const auto& output : m_PresentationLayout.outputs) {
        PlankOutputGeometry geometry;
        if (!outputGeometry(output.window, geometry)) {
            continue;
        }
        QPointF windowPoint;
        if (PlankPresentation::mapStreamPointToWindow(
                    streamPoint,
                    QSize(position.frameWidth, position.frameHeight),
                    geometry, windowPoint)) {
            window = output.window;
            x = qRound(windowPoint.x());
            y = qRound(windowPoint.y());
            return true;
        }
    }
    return false;
}

PlankWaylandCursor*
SdlInputHandler::ensureWaylandTabletCursorAttached(SDL_Window* targetWindow)
{
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!m_LocalCursorSupported || targetWindow == nullptr ||
            driver == nullptr || SDL_strcmp(driver, "wayland") != 0) {
        return nullptr;
    }

    auto existing = std::find_if(
            m_WaylandTabletCursorOutputs.begin(),
            m_WaylandTabletCursorOutputs.end(),
            [targetWindow](const WaylandTabletCursorOutput& output) {
                return output.window == targetWindow;
            });
    if (existing != m_WaylandTabletCursorOutputs.end() &&
            existing->cursor->isAttachedTo(targetWindow)) {
        return existing->cursor.get();
    }

    const bool replacing = existing != m_WaylandTabletCursorOutputs.end();
    if (replacing) {
        existing->cursor->setVisible(false);
        existing->cursor->dispatchPending();
        existing->cursor.reset();
    }

    std::unique_ptr<PlankWaylandCursor> cursor =
            PlankWaylandCursor::create(targetWindow);
    if (!cursor) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to attach PLANK Wayland Wacom cursor surface");
        if (replacing) {
            m_WaylandTabletCursorOutputs.erase(existing);
        }
        return nullptr;
    }

    if (m_AppliedRemoteCursorValid) {
        const QImage cursorImage(
                    m_AppliedRemoteCursor.pixels.data(),
                    static_cast<int>(m_AppliedRemoteCursor.width),
                    static_cast<int>(m_AppliedRemoteCursor.height),
                    static_cast<int>(m_AppliedRemoteCursor.width * 4U),
                    QImage::Format_ARGB32_Premultiplied);
        cursor->setImage(
                    cursorImage,
                    static_cast<int>(m_AppliedRemoteCursor.hotspotX),
                    static_cast<int>(m_AppliedRemoteCursor.hotspotY));
    }
    SDL_Window* positionWindow = nullptr;
    int positionX = 0;
    int positionY = 0;
    if (m_AppliedRemoteCursorPositionValid) {
        if (mapRemoteCursorPositionToWindow(
                    m_AppliedRemoteCursorPosition,
                    positionWindow, positionX, positionY) &&
                positionWindow == targetWindow) {
            cursor->setPosition(positionX, positionY);
        }
    }
    const bool visible = m_TabletCursorActive && isCaptureActive() &&
            m_RemoteCursorVisible && m_AppliedRemoteCursorPositionSequence >
                m_TabletCursorActivationSequence &&
            positionWindow == targetWindow;
    cursor->setVisible(visible);
    cursor->dispatchPending();

    PlankWaylandCursor* result = cursor.get();
    if (replacing) {
        existing->cursor = std::move(cursor);
    }
    else {
        m_WaylandTabletCursorOutputs.push_back(
                {targetWindow, std::move(cursor)});
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                replacing ?
                    "Reattached PLANK Wacom cursor to replacement Wayland parent surface for window %u" :
                    "PLANK Wayland Wacom cursor surface enabled for window %u",
                SDL_GetWindowID(targetWindow));
    return result;
}

void SdlInputHandler::reconcileWaylandTabletCursorOutputs()
{
    if (!m_LocalCursorSupported) {
        return;
    }

    const auto containsWindow = [this](SDL_Window* window) {
        if (m_PresentationLayout.outputs.isEmpty()) {
            return window == m_Window;
        }
        return std::any_of(
                m_PresentationLayout.outputs.cbegin(),
                m_PresentationLayout.outputs.cend(),
                [window](const PlankPresentationOutput& output) {
                    return output.window == window;
                });
    };
    auto output = m_WaylandTabletCursorOutputs.begin();
    while (output != m_WaylandTabletCursorOutputs.end()) {
        if (containsWindow(output->window)) {
            ++output;
            continue;
        }
        output->cursor->setVisible(false);
        output->cursor->dispatchPending();
        output = m_WaylandTabletCursorOutputs.erase(output);
    }

    if (m_PresentationLayout.outputs.isEmpty()) {
        ensureWaylandTabletCursorAttached(m_Window);
        return;
    }
    for (const auto& presentationOutput : m_PresentationLayout.outputs) {
        ensureWaylandTabletCursorAttached(presentationOutput.window);
    }
}

void SdlInputHandler::updateTabletCursorVisibility()
{
    updateSharedPointer();
    reconcileWaylandTabletCursorOutputs();

    SDL_Window* positionWindow = nullptr;
    int x = 0;
    int y = 0;
    if (m_AppliedRemoteCursorPositionValid) {
        mapRemoteCursorPositionToWindow(
                m_AppliedRemoteCursorPosition,
                positionWindow, x, y);
    }
    const bool visible = m_TabletCursorActive && isCaptureActive() &&
            m_RemoteCursorVisible &&
            m_AppliedRemoteCursorPositionSequence >
                m_TabletCursorActivationSequence;
#ifdef HAVE_MAC_RAW_WACOM
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window != positionWindow) MacWindow::hideTabletCursor(output.window);
    }
    if (positionWindow && m_AppliedRemoteCursorValid && (m_TabletCursorActive || m_SharedPointerTarget != positionWindow)) {
        const auto& image = m_AppliedRemoteCursor;
        MacWindow::tabletCursor(positionWindow, image.pixels.data(), image.width, image.height,
            image.hotspotX, image.hotspotY, image.generation, x, y, visible);
    }
#endif
    for (auto& output : m_WaylandTabletCursorOutputs) {
        output.cursor->setVisible(visible && output.window == positionWindow);
        output.cursor->dispatchPending();
    }
}

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)

void SdlInputHandler::refreshNormalizedPen()
{
    if (!m_SdlPenInput) return;
    bool raw = false;
#ifdef Q_OS_WIN
    raw = m_WindowsRawWacomInput && m_WindowsRawWacomInput->attached();
#endif
    bool focused = false;
    for (const auto& output : m_PresentationLayout.outputs) {
        if (output.window && (SDL_GetWindowFlags(output.window) & SDL_WINDOW_INPUT_FOCUS)) focused = true;
    }
    if (m_Window && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS)) focused = true;
    m_SdlPenInput->setActive(isCaptureActive() && focused && !raw);
}

bool SdlInputHandler::mapWindowPointToNormalizedStream(
        SDL_Window* window, float windowX, float windowY,
        float& normalizedX, float& normalizedY) const
{
    if (m_PresentationLayout.usesSourceRects()) {
        // Normalized over the whole workstation desktop.
        QPointF desktopPoint;
        if (!mapWindowPointToDesktop(window, windowX, windowY, desktopPoint, true)) {
            return false;
        }
        const QSize desktop = m_PresentationLayout.desktopSize;
        normalizedX = static_cast<float>(qBound(0.0, desktopPoint.x() / desktop.width(), 1.0));
        normalizedY = static_cast<float>(qBound(0.0, desktopPoint.y() / desktop.height(), 1.0));
        return true;
    }
    PlankOutputGeometry geometry;
    if (!outputGeometry(window, geometry)) {
        return false;
    }

    const QSize streamSize = streamDimensions();
    if (streamSize.width() <= 0 || streamSize.height() <= 0) {
        return false;
    }

    QPointF streamPoint;
    if (!PlankPresentation::mapWindowPointToStream(
                QPointF(windowX, windowY), geometry, streamSize,
                streamPoint, /*allowClampedPosition=*/true)) {
        return false;
    }

    normalizedX = static_cast<float>(
        qBound(0.0, streamPoint.x() / streamSize.width(), 1.0));
    normalizedY = static_cast<float>(
        qBound(0.0, streamPoint.y() / streamSize.height(), 1.0));
    return true;
}

void SdlInputHandler::handlePenProximityEvent(SDL_PenProximityEvent* event)
{
    refreshNormalizedPen();
    if (!m_SdlPenInput) {
        return;
    }
    if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
        m_SdlPenInput->handleProximityIn(event->which);
    }
    else {
        m_SdlPenInput->handleProximityOut(event->which);
    }
}

void SdlInputHandler::handlePenTouchEvent(SDL_PenTouchEvent* event)
{
    refreshNormalizedPen();
    if (!m_SdlPenInput) {
        return;
    }
    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }
    float normalizedX = 0.0f;
    float normalizedY = 0.0f;
    if (!mapWindowPointToNormalizedStream(window, event->x, event->y,
                                          normalizedX, normalizedY)) {
        return;
    }
    m_SdlPenInput->handleTouch(event->which, event->down, normalizedX,
                               normalizedY, event->pen_state);
}

void SdlInputHandler::handlePenMotionEvent(SDL_PenMotionEvent* event)
{
    refreshNormalizedPen();
    if (!m_SdlPenInput) {
        return;
    }
    SDL_Window* window = presentationWindow(event->windowID);
    if (window == nullptr) {
        return;
    }
    float normalizedX = 0.0f;
    float normalizedY = 0.0f;
    if (!mapWindowPointToNormalizedStream(window, event->x, event->y,
                                          normalizedX, normalizedY)) {
        return;
    }
    m_SdlPenInput->handlePosition(event->which, normalizedX, normalizedY,
                                  event->pen_state);
}

void SdlInputHandler::handlePenButtonEvent(SDL_PenButtonEvent* event)
{
    refreshNormalizedPen();
    if (!m_SdlPenInput) {
        return;
    }
    // x, y, pressure, tilt and rotation are ignored host-side for
    // LI_TOUCH_EVENT_BUTTON_ONLY (see Limelight.h and macpen.h), so this
    // does not need to map event->x/y through the presentation layout.
    m_SdlPenInput->handleButton(event->which, event->pen_state);
}

void SdlInputHandler::handlePenAxisEvent(SDL_PenAxisEvent* event)
{
    refreshNormalizedPen();
    if (!m_SdlPenInput) {
        return;
    }
    if (static_cast<int>(event->axis) < 0 ||
            static_cast<int>(event->axis) >
                static_cast<int>(PlankMacPenAxis::TangentialPressure)) {
        return;
    }
    m_SdlPenInput->handleAxis(event->which,
                              static_cast<PlankMacPenAxis>(event->axis),
                              event->value);
}

#endif // Q_OS_MACOS || Q_OS_WIN

void SdlInputHandler::hideSharedPointer()
{
#ifdef Q_OS_MACOS
    if (m_SharedPointerTargetId && SDL_GetWindowFromID(m_SharedPointerTargetId) == m_SharedPointerTarget)
        MacWindow::hideTabletCursor(m_SharedPointerTarget);
#endif
    if (m_SharedPointerWindow) m_SharedPointerWindow->hide();
    if (m_SharedWaylandPointer) {
        m_SharedWaylandPointer->setVisible(false);
        m_SharedWaylandPointer->dispatchPending();
    }
    m_SharedPointerTarget = nullptr;
    m_SharedPointerTargetId = 0;
}

void SdlInputHandler::updateSharedPointer()
{
    SDL_Window* window = nullptr;
    int x = 0, y = 0;
    bool focused = false;
    for (const auto& output : m_PresentationLayout.outputs) {
#ifdef Q_OS_MACOS
        focused |= MacWindow::hasKeyboardFocus(output.window);
#else
        focused |= (SDL_GetWindowFlags(output.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
    }
    if (!m_AppliedRemoteCursorPositionValid ||
            !mapRemoteCursorPositionToWindow(m_AppliedRemoteCursorPosition, window, x, y) ||
            !plankSharedPointerVisible(focused, m_TabletCursorActive, m_RemoteCursorVisible,
                m_ViewOnly, window && SDL_GetWindowID(window) == m_LastLocalPointerWindow,
                QPointF(x, y), m_LastLocalPointer, SDL_GetTicks() - m_LastLocalPointerMotion) ||
            (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))) {
        hideSharedPointer();
        return;
    }
    if (m_SharedPointerTarget != window) hideSharedPointer();
    static const QImage image = plankSharedPointerImage();
#ifdef Q_OS_MACOS
    MacWindow::tabletCursor(window, image.constBits(), image.width(), image.height(),
                           1, 1, UINT64_MAX, x, y, true);
#else
    if (qstrcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        if (!m_SharedWaylandPointer || !m_SharedWaylandPointer->isAttachedTo(window)) {
            m_SharedWaylandPointer = PlankWaylandCursor::create(window);
            if (!m_SharedWaylandPointer) return;
            m_SharedWaylandPointer->setImage(image, 1, 1);
        }
        m_SharedWaylandPointer->setPosition(x, y);
        m_SharedWaylandPointer->setVisible(true);
        m_SharedWaylandPointer->dispatchPending();
    } else {
        int left = 0, top = 0;
        if (!SDL_GetWindowPosition(window, &left, &top)) return;
        if (!m_SharedPointerWindow) m_SharedPointerWindow = std::make_unique<PlankSharedPointerOverlay>();
        m_SharedPointerWindow->present(QPoint(left + x - 1, top + y - 1), true);
    }
#endif
    m_SharedPointerTarget = window;
    m_SharedPointerTargetId = SDL_GetWindowID(window);
}
