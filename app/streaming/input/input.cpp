#include <Limelight.h>
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

    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCombo = KeyComboToggleCursorHide;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].keyCode = SDLK_C;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].scanCode = SDL_SCANCODE_C;
    m_SpecialKeyCombos[KeyComboToggleCursorHide].enabled = true;

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
    // The StationConnect toolbar draws its own pointer from the exact native
    // popup coordinates used for hit testing. Keep SDL's Wayland cursor hidden
    // while the popup owns input so there is never a second cursor with a
    // different ownership history. When routing returns to the parent window,
    // restore the normal video/letterbox visibility policy.
    setCursorVisible(active ? false :
                     (!m_MouseWasInVideoRegion ||
                      m_MouseCursorCapturedVisibilityState));
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
        setCursorVisible(m_MouseCursorCapturedVisibilityState);
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
