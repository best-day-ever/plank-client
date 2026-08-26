#pragma once

#include <SDL3/SDL.h>

#include <memory>

#include "stationconnecttoolbarlogic.h"

class StreamingPreferences;
class SdlInputHandler;
class StationConnectWaylandToolbar;

namespace Overlay {
class OverlayManager;
}

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
                          Overlay::OverlayManager& overlayManager,
                          SdlInputHandler& inputHandler,
                          StreamingPreferences& preferences);
    ~StationConnectToolbar();

    void setRenderedStats(float fps, float videoMbps, float packetLossPercent);
    void setAppliedBitrate(int requestedKbps, int appliedKbps, int peakKbps);
    Action update(Uint64 now);
    void notifyWindowChanged();
    void notifyFocusLost();

    void observeMouseMotion(const SDL_MouseMotionEvent& event);
    Action handleMouseButton(const SDL_MouseButtonEvent& event);
    bool handleMouseWheel(const SDL_MouseWheelEvent& event);

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
    void beginLocalPointerInteraction();
    void endLocalPointerInteraction();
    void createWaylandToolbar();
    void redraw();
    void updateBitrateFromPointer(int x, Uint64 now, bool forceSend);
    void queueBitrateRequest(Uint64 now, bool forceSend);
    void nativePointerEnter(int localX, int localY);
    void nativePointerLeave();
    void nativePointerMotion(int localX, int localY);
    void nativePointerButton(uint32_t button, bool down);
    void nativePointerWheel(int verticalSteps);
    void forwardNativePointerPosition();
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
    Overlay::OverlayManager& m_OverlayManager;
    SdlInputHandler& m_InputHandler;
    StreamingPreferences& m_Preferences;
    std::unique_ptr<StationConnectWaylandToolbar> m_WaylandToolbar;
    Action m_PendingAction;
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
    int m_ToolbarDragOffsetX;
    int m_ToolbarDragStartLeft;
    int m_PendingToolbarLeft;
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
