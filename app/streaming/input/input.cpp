#include <Limelight.h>
#include <StationConnect.h>
#include <SDL3/SDL.h>
#include "streaming/input/input.h"
#include "streaming/session.h"
#include "utils.h"

#ifdef HAVE_LIBINPUT_TABLET
#include "streaming/input/linuxwacom.h"
#include "streaming/input/linuxrawwacom.h"
#endif

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>
#include <QtEndian>

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
      m_FakeMouseCaptureActive(false),
      m_KeyboardCaptureActive(false),
      m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_MouseCursorCapturedVisibilityState(false),
      m_StreamWidth(streamWidth),
      m_StreamHeight(streamHeight)
{
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
    m_SpecialKeyCombos[KeyComboQuit].keyCombo = KeyComboQuit;
    m_SpecialKeyCombos[KeyComboQuit].keyCode = SDLK_Q;
    m_SpecialKeyCombos[KeyComboQuit].scanCode = SDL_SCANCODE_Q;
    m_SpecialKeyCombos[KeyComboQuit].enabled = true;

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
}

SdlInputHandler::~SdlInputHandler()
{
#ifdef HAVE_LIBINPUT_TABLET
    m_LinuxWacomInput.reset();
    m_LinuxRawWacomInput.reset();
#endif

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
                    "StationConnect local cursor transport enabled");
        setCursorVisible(true);
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                     "Host does not support required StationConnect local cursor transport");
    }
#ifdef HAVE_LIBINPUT_TABLET
    if (qEnvironmentVariableIntValue("STATIONCONNECT_EXTERNAL_WACOM_BRIDGE") != 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                    "Normalized Wacom capture disabled for external raw-HID qualification");
    }
    else if ((LiGetHostFeatureFlags() &
              (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) ==
             (LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND)) {
        m_LinuxRawWacomInput.reset(new LinuxRawWacomInput());
        m_LinuxRawWacomInput->setActive(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
    else if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
        m_LinuxWacomInput.reset(new LinuxWacomInput());
        m_LinuxWacomInput->setActive(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }
#endif
}

bool SdlInputHandler::handleRemoteCursorChunk(const unsigned char* data,
                                              unsigned int length)
{
    if (data == nullptr || length < sizeof(SC_CURSOR_WIRE_HEADER) ||
            length > sizeof(SC_CURSOR_WIRE_HEADER) + SC_CURSOR_MAX_CHUNK_SIZE) {
        return false;
    }

    SC_CURSOR_WIRE_HEADER wire;
    std::memcpy(&wire, data, sizeof(wire));
    const std::uint32_t magic = qFromLittleEndian(wire.magic);
    const std::uint16_t version = qFromLittleEndian(wire.version);
    const std::uint16_t pixelFormat = qFromLittleEndian(wire.pixelFormat);
    const std::uint32_t flags = qFromLittleEndian(wire.flags);
    const std::uint64_t generation = qFromLittleEndian(wire.generation);
    const std::uint32_t width = qFromLittleEndian(wire.width);
    const std::uint32_t height = qFromLittleEndian(wire.height);
    const std::uint32_t hotspotX = qFromLittleEndian(wire.hotspotX);
    const std::uint32_t hotspotY = qFromLittleEndian(wire.hotspotY);
    const std::uint32_t imageSize = qFromLittleEndian(wire.imageSize);
    const std::uint32_t chunkOffset = qFromLittleEndian(wire.chunkOffset);
    const std::uint32_t chunkSize = qFromLittleEndian(wire.chunkSize);
    constexpr std::uint32_t knownFlags = SC_CURSOR_FLAG_VISIBLE |
                                          SC_CURSOR_FLAG_FIRST_CHUNK |
                                          SC_CURSOR_FLAG_LAST_CHUNK;

    if (magic != SC_CURSOR_WIRE_MAGIC || version != SC_CURSOR_WIRE_VERSION ||
            pixelFormat != SC_CURSOR_PIXEL_FORMAT_ARGB8888 ||
            (flags & ~knownFlags) != 0 || width == 0 || height == 0 ||
            width > SC_CURSOR_MAX_DIMENSION || height > SC_CURSOR_MAX_DIMENSION ||
            imageSize != width * height * 4U || imageSize > SC_CURSOR_MAX_IMAGE_SIZE ||
            hotspotX >= width || hotspotY >= height ||
            chunkSize > SC_CURSOR_MAX_CHUNK_SIZE ||
            sizeof(wire) + chunkSize != length ||
            chunkOffset > imageSize || chunkSize > imageSize - chunkOffset) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Rejected malformed StationConnect local cursor chunk");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_RemoteCursorMutex);
    if ((flags & SC_CURSOR_FLAG_FIRST_CHUNK) != 0) {
        if (chunkOffset != 0) {
            return false;
        }
        m_RemoteCursorAssembly = {};
        m_RemoteCursorAssembly.generation = generation;
        m_RemoteCursorAssembly.width = width;
        m_RemoteCursorAssembly.height = height;
        m_RemoteCursorAssembly.hotspotX = hotspotX;
        m_RemoteCursorAssembly.hotspotY = hotspotY;
        m_RemoteCursorAssembly.flags = flags & SC_CURSOR_FLAG_VISIBLE;
        m_RemoteCursorAssembly.pixels.resize(imageSize);
        m_RemoteCursorAssemblyActive = true;
    }

    if (!m_RemoteCursorAssemblyActive ||
            generation != m_RemoteCursorAssembly.generation ||
            width != m_RemoteCursorAssembly.width ||
            height != m_RemoteCursorAssembly.height ||
            hotspotX != m_RemoteCursorAssembly.hotspotX ||
            hotspotY != m_RemoteCursorAssembly.hotspotY ||
            (flags & SC_CURSOR_FLAG_VISIBLE) != m_RemoteCursorAssembly.flags ||
            chunkOffset != m_RemoteCursorAssembly.nextOffset) {
        m_RemoteCursorAssemblyActive = false;
        return false;
    }

    std::memcpy(m_RemoteCursorAssembly.pixels.data() + chunkOffset,
                data + sizeof(wire), chunkSize);
    m_RemoteCursorAssembly.nextOffset += chunkSize;

    if ((flags & SC_CURSOR_FLAG_LAST_CHUNK) == 0) {
        return false;
    }
    if (m_RemoteCursorAssembly.nextOffset != imageSize) {
        m_RemoteCursorAssemblyActive = false;
        return false;
    }

    m_ReadyRemoteCursor = std::move(m_RemoteCursorAssembly);
    m_ReadyRemoteCursorValid = true;
    m_RemoteCursorAssemblyActive = false;
    return !m_RemoteCursorUpdatePending.exchange(true);
}

void SdlInputHandler::applyPendingRemoteCursor()
{
    RemoteCursorState cursor;
    {
        std::lock_guard<std::mutex> lock(m_RemoteCursorMutex);
        if (!m_ReadyRemoteCursorValid) {
            m_RemoteCursorUpdatePending.store(false);
            return;
        }
        cursor = std::move(m_ReadyRemoteCursor);
        m_ReadyRemoteCursorValid = false;
        m_RemoteCursorUpdatePending.store(false);
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
                static_cast<int>(cursor.width),
                static_cast<int>(cursor.height),
                SDL_PIXELFORMAT_ARGB8888,
                cursor.pixels.data(),
                static_cast<int>(cursor.width * 4U));
    if (surface == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Failed to create StationConnect cursor surface: %s",
                    SDL_GetError());
        return;
    }

    SDL_Cursor* replacement = SDL_CreateColorCursor(
                surface,
                static_cast<int>(cursor.hotspotX),
                static_cast<int>(cursor.hotspotY));
    SDL_DestroySurface(surface);
    if (replacement == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Failed to create StationConnect compositor cursor: %s",
                    SDL_GetError());
        return;
    }

    const bool firstCursor = m_RemoteCursor == nullptr;
    SDL_SetCursor(replacement);
    if (m_RemoteCursor != nullptr) {
        SDL_DestroyCursor(m_RemoteCursor);
    }
    m_RemoteCursor = replacement;
    m_RemoteCursorVisible =
            (cursor.flags & SC_CURSOR_FLAG_VISIBLE) != 0;
    if (isCaptureActive()) {
        setCursorVisible(!m_MouseWasInVideoRegion || m_RemoteCursorVisible);
    }

    if (firstCursor) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Applied first StationConnect local cursor (%ux%u hotspot %u,%u visible=%d)",
                    cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                    m_RemoteCursorVisible ? 1 : 0);
    }
    else {
        SDL_LogDebug(SDL_LOG_CATEGORY_INPUT,
                     "Applied StationConnect local cursor generation %llu (%ux%u hotspot %u,%u visible=%d)",
                     static_cast<unsigned long long>(cursor.generation),
                     cursor.width, cursor.height, cursor.hotspotX, cursor.hotspotY,
                     m_RemoteCursorVisible ? 1 : 0);
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
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(false);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(false);
    }
#endif

    // Raise all keys that are currently pressed. If we don't do this, certain keys
    // used in shortcuts that cause focus loss (such as Alt+Tab) may get stuck down.
    raiseAllKeys();
}

void SdlInputHandler::notifyFocusGained()
{
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxWacomInput) {
        m_LinuxWacomInput->setActive(true);
    }
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->setActive(true);
    }
#endif
}

void SdlInputHandler::handleRawHidControl(const unsigned char* data,
                                          unsigned int length)
{
#ifdef HAVE_LIBINPUT_TABLET
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
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->beginReconnect();
    }
#endif
}

void SdlInputHandler::finishRawHidReconnect()
{
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->finishReconnect();
    }
#endif
}

bool SdlInputHandler::isCaptureActive()
{
    return m_FakeMouseCaptureActive;
}

void SdlInputHandler::setToolbarInteractionActive(bool active)
{
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

    SDL_SetWindowKeyboardGrab(m_Window, shouldGrab ? true : false);

    m_KeyboardCaptureActive = shouldGrab;
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
    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
    if (!(windowFlags & SDL_WINDOW_INPUT_FOCUS) || !m_KeyboardCaptureActive) {
        return false;
    }

    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
        return false;
    }

    return true;
}

void SdlInputHandler::setCaptureActive(bool active)
{
    if (active) {
        setCursorVisible(m_LocalCursorSupported ?
                             (!m_MouseWasInVideoRegion || m_RemoteCursorVisible) :
                             m_MouseCursorCapturedVisibilityState);
        m_FakeMouseCaptureActive = true;

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

        if (isMouseInVideoRegion(mouseX, mouseY)) {
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
        setCursorVisible(true);
        m_FakeMouseCaptureActive = false;
    }

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}

void SdlInputHandler::setCursorVisible(bool visible)
{
    if (visible) {
        SDL_ShowCursor();
    }
    else {
        SDL_HideCursor();
    }
}
