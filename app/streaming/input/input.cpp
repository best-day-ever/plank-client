#include <Limelight.h>
#include <SDL.h>
#include "streaming/input/input.h"
#include "utils.h"

#ifdef HAVE_LIBINPUT_TABLET
#include "streaming/input/linuxwacom.h"
#include "streaming/input/linuxrawwacom.h"
#endif

#include <QtGlobal>
#include <QDir>
#include <QGuiApplication>

SdlInputHandler::SdlInputHandler(StreamingPreferences& prefs,
                                 int streamWidth,
                                 int streamHeight)
    : m_MouseWasInVideoRegion(false),
      m_PendingMouseButtonsAllUpOnVideoRegionLeave(false),
      m_PointerRegionLockActive(false),
      m_PointerRegionLockToggledByUser(false),
      m_FakeCaptureActive(false),
      m_CaptureSystemKeysMode(prefs.captureSysKeysMode),
      m_MouseCursorCapturedVisibilityState(SDL_DISABLE),
      m_StreamWidth(streamWidth),
      m_StreamHeight(streamHeight)
{
    // System keys are always captured when running without a DE
    if (!WMUtils::isRunningDesktopEnvironment()) {
        m_CaptureSystemKeysMode = StreamingPreferences::CSK_ALWAYS;
    }

#if !SDL_VERSION_ATLEAST(2, 0, 15)
    // For older versions of SDL (2.0.14 and earlier), use SDL_HINT_GRAB_KEYBOARD
    SDL_SetHintWithPriority(SDL_HINT_GRAB_KEYBOARD,
                            m_CaptureSystemKeysMode != StreamingPreferences::CSK_OFF ? "1" : "0",
                            SDL_HINT_OVERRIDE);
#endif

    // Opt-out of SDL's built-in Alt+Tab handling while keyboard grab is enabled
    SDL_SetHint("SDL_ALLOW_ALT_TAB_WHILE_GRABBED", "0");

    // Allow clicks to pass through to us when focusing the window. If we're in
    // absolute mouse mode, this will avoid the user having to click twice to
    // trigger a click on the host if the Moonlight window is not focused. In
    // relative mode, the click event will trigger the mouse to be recaptured.
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // Populate special key combo configuration
    m_SpecialKeyCombos[KeyComboQuit].keyCombo = KeyComboQuit;
    m_SpecialKeyCombos[KeyComboQuit].keyCode = SDLK_q;
    m_SpecialKeyCombos[KeyComboQuit].scanCode = SDL_SCANCODE_Q;
    m_SpecialKeyCombos[KeyComboQuit].enabled = true;

    m_SpecialKeyCombos[KeyComboUngrabInput].keyCombo = KeyComboUngrabInput;
    m_SpecialKeyCombos[KeyComboUngrabInput].keyCode = SDLK_z;
    m_SpecialKeyCombos[KeyComboUngrabInput].scanCode = SDL_SCANCODE_Z;
    m_SpecialKeyCombos[KeyComboUngrabInput].enabled = QGuiApplication::platformName() != "eglfs";

    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCombo = KeyComboToggleFullScreen;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].keyCode = SDLK_x;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].scanCode = SDL_SCANCODE_X;
    m_SpecialKeyCombos[KeyComboToggleFullScreen].enabled = QGuiApplication::platformName() != "eglfs";

    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCombo = KeyComboToggleStatsOverlay;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].keyCode = SDLK_s;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].scanCode = SDL_SCANCODE_S;
    m_SpecialKeyCombos[KeyComboToggleStatsOverlay].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCombo = KeyComboToggleCursorHide;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCode = SDLK_c;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].scanCode = SDL_SCANCODE_C;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].enabled = true;

    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCombo = KeyComboToggleMinimize;
    m_SpecialKeyCombos[KeyComboToggleMinimize].keyCode = SDLK_d;
    m_SpecialKeyCombos[KeyComboToggleMinimize].scanCode = SDL_SCANCODE_D;
    m_SpecialKeyCombos[KeyComboToggleMinimize].enabled = QGuiApplication::platformName() != "eglfs";

    m_SpecialKeyCombos[KeyComboPasteText].keyCombo = KeyComboPasteText;
    m_SpecialKeyCombos[KeyComboPasteText].keyCode = SDLK_v;
    m_SpecialKeyCombos[KeyComboPasteText].scanCode = SDL_SCANCODE_V;
    m_SpecialKeyCombos[KeyComboPasteText].enabled = true;

    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCombo = KeyComboTogglePointerRegionLock;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].keyCode = SDLK_l;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].scanCode = SDL_SCANCODE_L;
    m_SpecialKeyCombos[KeyComboTogglePointerRegionLock].enabled = true;

}

SdlInputHandler::~SdlInputHandler()
{
#ifdef HAVE_LIBINPUT_TABLET
    m_LinuxWacomInput.reset();
    m_LinuxRawWacomInput.reset();
#endif

#ifdef STEAM_LINK
    // Hide SDL's cursor on Steam Link after quitting the stream.
    // FIXME: We should also do this for other situations where SDL
    // and Qt will draw their own mouse cursors like KMSDRM or RPi
    // video backends.
    SDL_ShowCursor(SDL_DISABLE);
#endif
}

void SdlInputHandler::setWindow(SDL_Window *window)
{
    m_Window = window;
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

void SdlInputHandler::raiseAllKeys()
{
    if (m_KeysDown.isEmpty()) {
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Raising %d keys",
                (int)m_KeysDown.count());

    for (auto keyDown : m_KeysDown) {
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
    if (isCaptureActive()) {
        // NB: Not using SDL_GetGlobalMouseState() because we want our state not the system's
        Uint32 mouseState = SDL_GetMouseState(nullptr, nullptr);
        for (Uint32 button = SDL_BUTTON_LEFT; button <= SDL_BUTTON_X2; button++) {
            if (mouseState & SDL_BUTTON(button)) {
                SDL_CaptureMouse(SDL_TRUE);
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

void SdlInputHandler::resetRawHidAfterReconnect()
{
#ifdef HAVE_LIBINPUT_TABLET
    if (m_LinuxRawWacomInput) {
        m_LinuxRawWacomInput->resetAfterReconnect();
    }
#endif
}

bool SdlInputHandler::isCaptureActive()
{
    return m_FakeCaptureActive;
}

void SdlInputHandler::setToolbarInteractionActive(bool active)
{
    // Absolute desktop mode does not need a capture transition. Show the
    // receiver cursor only while receiver UI owns input, then restore the
    // user's captured-cursor state when routing resumes to the host.
    SDL_ShowCursor(active ? SDL_ENABLE : m_MouseCursorCapturedVisibilityState);
}

void SdlInputHandler::updateKeyboardGrabState()
{
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_OFF) {
        return;
    }

    bool shouldGrab = isCaptureActive();
    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
    if (m_CaptureSystemKeysMode == StreamingPreferences::CSK_FULLSCREEN &&
            !(windowFlags & SDL_WINDOW_FULLSCREEN)) {
        // Ungrab if it's fullscreen only and we left fullscreen
        shouldGrab = false;
    }

    // Don't close the window on Alt+F4 when keyboard grab is enabled
    SDL_SetHint(SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4, shouldGrab ? "1" : "0");

#if SDL_VERSION_ATLEAST(2, 0, 15)
    // On SDL 2.0.15+, we can get keyboard-only grab on Win32, X11, and Wayland.
    // SDL 2.0.18 adds keyboard grab on macOS (if built with non-AppStore APIs).
    SDL_SetWindowKeyboardGrab(m_Window, shouldGrab ? SDL_TRUE : SDL_FALSE);
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

    Uint32 windowFlags = SDL_GetWindowFlags(m_Window);
    if (!(windowFlags & SDL_WINDOW_INPUT_FOCUS)
#if SDL_VERSION_ATLEAST(2, 0, 15)
            || !(windowFlags & SDL_WINDOW_KEYBOARD_GRABBED)
#else
            || !(windowFlags & SDL_WINDOW_INPUT_GRABBED)
#endif
            )
    {
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
        SDL_ShowCursor(m_MouseCursorCapturedVisibilityState);
        m_FakeCaptureActive = true;

        // Synchronize the client and host cursor when activating absolute capture
        int mouseX, mouseY;
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
            motionEvent.type = SDL_MOUSEMOTION;
            motionEvent.timestamp = SDL_GetTicks();
            motionEvent.windowID = SDL_GetWindowID(m_Window);
            motionEvent.x = mouseX;
            motionEvent.y = mouseY;
            handleMouseMotionEvent(&motionEvent);
        }
    }
    else {
        SDL_ShowCursor(SDL_ENABLE);
        m_FakeCaptureActive = false;
    }

    // Update mouse pointer region constraints
    updatePointerRegionLock();

    // Now update the keyboard grab
    updateKeyboardGrabState();
}
