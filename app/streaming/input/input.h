#pragma once

#include "settings/streamingpreferences.h"
#include "backend/computermanager.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

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

    bool handleRemoteCursorChunk(const unsigned char* data, unsigned int length);

    void applyPendingRemoteCursor();

    void beginRawHidReconnect();
    void finishRawHidReconnect();

    void raiseAllKeys();

    void notifyMouseLeave();

    void notifyFocusLost();

    void notifyFocusGained();

    bool isCaptureActive();

    void setToolbarInteractionActive(bool active);

    void setLocalToolbarAvailable(bool available);

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
    bool m_LocalToolbarAvailable;
    bool m_LocalCursorSupported;
    bool m_RemoteCursorVisible;

    QSet<short> m_KeysDown;
    bool m_FakeMouseCaptureActive;
    bool m_KeyboardCaptureActive;
    StreamingPreferences::CaptureSysKeysMode m_CaptureSystemKeysMode;
    bool m_MouseCursorCapturedVisibilityState;

    struct RemoteCursorState {
        std::uint64_t generation = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t hotspotX = 0;
        std::uint32_t hotspotY = 0;
        std::uint32_t flags = 0;
        std::uint32_t nextOffset = 0;
        std::vector<unsigned char> pixels;
    };

    std::mutex m_RemoteCursorMutex;
    RemoteCursorState m_RemoteCursorAssembly;
    RemoteCursorState m_ReadyRemoteCursor;
    bool m_RemoteCursorAssemblyActive = false;
    bool m_ReadyRemoteCursorValid = false;
    std::atomic_bool m_RemoteCursorUpdatePending {false};
    SDL_Cursor* m_RemoteCursor = nullptr;

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
