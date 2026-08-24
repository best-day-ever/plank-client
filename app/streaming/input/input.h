#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include <SDL3/SDL.h>

#ifdef HAVE_LIBINPUT_TABLET
#include <memory>
class LinuxWacomInput;
class LinuxRawWacomInput;
#endif

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs,
                             int streamWidth,
                             int streamHeight);

    ~SdlInputHandler();

    void setWindow(SDL_Window* window);

    void handleKeyEvent(SDL_KeyboardEvent* event);

    void handleMouseButtonEvent(SDL_MouseButtonEvent* event);

    void handleMouseMotionEvent(SDL_MouseMotionEvent* event,
                                bool batchPendingEvents = true);

    void handleMouseWheelEvent(SDL_MouseWheelEvent* event);

    void sendText(QString& string);

    void handleRawHidControl(const unsigned char* data, unsigned int length);

    void resetRawHidAfterReconnect();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    void setToolbarInteractionActive(bool active);

    bool isSystemKeyCaptureActive();

    void setCaptureActive(bool active);

    bool isMouseInVideoRegion(int mouseX, int mouseY, int windowWidth = -1, int windowHeight = -1);

    void updateKeyboardGrabState();

    void updatePointerRegionLock();

private:
    enum KeyCombo {
        KeyComboQuit,
        KeyComboUngrabInput,
        KeyComboToggleFullScreen,
        KeyComboToggleStatsOverlay,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboToggleKeyboardGrab,
        KeyComboMax
    };

    void performSpecialKeyCombo(KeyCombo combo);

    SDL_Window* m_Window;
    bool m_NeedsManualCaptureOnLeave;
    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    QSet<short> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    bool m_MouseCursorCapturedVisibilityState;

    void setCursorVisible(bool visible);

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    int m_StreamWidth;
    int m_StreamHeight;

#ifdef HAVE_LIBINPUT_TABLET
    std::unique_ptr<LinuxWacomInput> m_LinuxWacomInput;
    std::unique_ptr<LinuxRawWacomInput> m_LinuxRawWacomInput;
#endif

    static const int k_ButtonMap[];
};
