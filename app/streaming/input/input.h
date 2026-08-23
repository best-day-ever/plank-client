#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include <SDL.h>

#ifdef HAVE_LIBINPUT_TABLET
#include <memory>
class LinuxWacomInput;
class LinuxRawWacomInput;
#endif

#define MAX_FINGERS 2

class SdlInputHandler
{
public:
    explicit SdlInputHandler(StreamingPreferences& prefs,
                             int streamWidth,
                             int streamHeight,
                             bool forceAbsoluteMouseMode = false);

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

    void handleTouchFingerEvent(SDL_TouchFingerEvent* event);

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    bool isAbsoluteMouseMode() const;

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
        KeyComboToggleMouseMode,
        KeyComboToggleCursorHide,
        KeyComboToggleMinimize,
        KeyComboPasteText,
        KeyComboTogglePointerRegionLock,
        KeyComboMax
    };

    void handleAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void emulateAbsoluteFingerEvent(SDL_TouchFingerEvent* event);

    void disableTouchFeedback();

    void handleRelativeFingerEvent(SDL_TouchFingerEvent* event);

    void performSpecialKeyCombo(KeyCombo combo);

    static
    Uint32 longPressTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseLeftButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 releaseRightButtonTimerCallback(Uint32 interval, void* param);

    static
    Uint32 dragTimerCallback(Uint32 interval, void* param);

    SDL_Window* m_Window;
    bool m_SwapMouseButtons;
    bool m_ReverseScrollDirection;

    bool m_MouseWasInVideoRegion;
    bool m_PendingMouseButtonsAllUpOnVideoRegionLeave;
    bool m_PointerRegionLockActive;
    bool m_PointerRegionLockToggledByUser;

    QSet<short> m_KeysDown;
    bool m_FakeCaptureActive;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    int m_MouseCursorCapturedVisibilityState;

    struct {
        KeyCombo keyCombo;
        SDL_Keycode keyCode;
        SDL_Scancode scanCode;
        bool enabled;
    } m_SpecialKeyCombos[KeyComboMax];

    SDL_TouchFingerEvent m_LastTouchDownEvent;
    SDL_TouchFingerEvent m_LastTouchUpEvent;
    SDL_TimerID m_LongPressTimer;
    int m_StreamWidth;
    int m_StreamHeight;
    bool m_AbsoluteMouseMode;
    bool m_AbsoluteTouchMode;
    bool m_DisabledTouchFeedback;

    SDL_TouchFingerEvent m_TouchDownEvent[MAX_FINGERS];
    SDL_TimerID m_LeftButtonReleaseTimer;
    SDL_TimerID m_RightButtonReleaseTimer;
    SDL_TimerID m_DragTimer;
    char m_DragButton;
    int m_NumFingersDown;

#ifdef HAVE_LIBINPUT_TABLET
    std::unique_ptr<LinuxWacomInput> m_LinuxWacomInput;
    std::unique_ptr<LinuxRawWacomInput> m_LinuxRawWacomInput;
#endif

    static const int k_ButtonMap[];
};
