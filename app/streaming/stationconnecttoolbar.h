#pragma once

#include <SDL3/SDL.h>

#include "stationconnecttoolbarlogic.h"

class StreamingPreferences;
class SdlInputHandler;

class StationConnectToolbar
{
public:
    enum class Action {
        None,
        Consumed,
        ToggleFullscreen,
        Minimize,
        Disconnect,
    };

    StationConnectToolbar(SDL_Window* window,
                          SdlInputHandler& inputHandler,
                          StreamingPreferences& preferences);
    ~StationConnectToolbar();

    void setRenderedStats(float fps, float videoMbps, float packetLossPercent);
    void setAppliedBitrate(int requestedKbps, int appliedKbps, int peakKbps);
    void update(Uint64 now);
    void notifyWindowChanged();
    void notifyFocusLost();
    bool handleWindowEvent(const SDL_Event& event);

    bool handleMouseMotion(SDL_MouseMotionEvent& event);
    Action handleMouseButton(SDL_MouseButtonEvent& event);
    bool handleMouseWheel(SDL_MouseWheelEvent& event);

    int eventWaitTimeout() const;

private:
    enum class Control {
        None,
        Handle,
        Slider,
        Pin,
        Fullscreen,
        Minimize,
        Disconnect,
    };

    void show(Uint64 now);
    void hide();
    bool createToolbarWindow();
    void positionToolbarWindow();
    void updateToolbarWindowMetrics();
    void beginLocalPointerInteraction();
    void endLocalPointerInteraction();
    void redraw();
    void updateBitrateFromPointer(int x, Uint64 now, bool forceSend);
    void queueBitrateRequest(Uint64 now, bool forceSend);
    bool contains(int x, int y) const;
    bool sliderContains(int x, int y) const;
    bool handleContains(int x, int y) const;
    bool pinContains(int x, int y) const;
    bool fullscreenContains(int x, int y) const;
    bool minimizeContains(int x, int y) const;
    bool disconnectContains(int x, int y) const;
    Control controlAt(int x, int y) const;
    int toolbarLeft() const;
    int sliderLeft() const;
    int sliderRight() const;

    SDL_Window* m_Window;
    SDL_Window* m_ToolbarWindow;
    SDL_WindowID m_ToolbarWindowId;
    SdlInputHandler& m_InputHandler;
    StreamingPreferences& m_Preferences;
    bool m_Visible;
    bool m_Pinned;
    bool m_DraggingToolbar;
    bool m_DraggingSlider;
    bool m_PointerInside;
    bool m_PointerInitialized;
    bool m_LocalPointerInteraction;
    bool m_BitrateSupported;
    int m_WindowWidth;
    int m_WindowHeight;
    int m_WindowPixelWidth;
    int m_WindowPixelHeight;
    float m_PixelDensity;
    int m_Width;
    int m_ToolbarLeft;
    int m_PointerX;
    int m_PointerY;
    Control m_PressedControl;
    StationConnectToolbarLogic::ButtonRouter m_ButtonRouter;
    int m_BitrateKbps;
    int m_LastSentBitrateKbps;
    int m_AppliedBitrateKbps;
    int m_AppliedPeakKbps;
    float m_RenderedFps;
    float m_VideoMbps;
    float m_PacketLossPercent;
    float m_LastDrawnFps;
    float m_LastDrawnVideoMbps;
    float m_LastDrawnPacketLossPercent;
    Uint64 m_HideDeadline;
    Uint64 m_LastBitrateSendTime;
    Uint64 m_LastBitrateChangeTime;
    Uint64 m_LastToolbarMoveDrawTime;
    Uint64 m_LastRedrawTime;
    Uint64 m_EdgeHoverStartTime;
};
