#pragma once

#include <SDL.h>

class StreamingPreferences;
class SdlInputHandler;

namespace Overlay {
class OverlayManager;
}

class StationConnectToolbar
{
public:
    enum class Action {
        None,
        Consumed,
        Minimize,
        Disconnect,
    };

    StationConnectToolbar(SDL_Window* window,
                          Overlay::OverlayManager& overlayManager,
                          SdlInputHandler& inputHandler,
                          StreamingPreferences& preferences);
    ~StationConnectToolbar();

    void setRenderedStats(float fps, float videoMbps);
    void setAppliedBitrate(int requestedKbps, int appliedKbps, int peakKbps);
    void update(Uint32 now);
    void notifyWindowChanged();

    bool handleMouseMotion(const SDL_MouseMotionEvent& event);
    Action handleMouseButton(const SDL_MouseButtonEvent& event);
    bool handleMouseWheel(const SDL_MouseWheelEvent& event);

    int eventWaitTimeout() const;

private:
    void show(Uint32 now);
    void hide();
    void beginLocalPointerInteraction();
    void endLocalPointerInteraction();
    void redraw();
    void updateBitrateFromPointer(int x, Uint32 now, bool forceSend);
    void queueBitrateRequest(Uint32 now, bool forceSend);
    bool contains(int x, int y) const;
    bool sliderContains(int x, int y) const;
    bool handleContains(int x, int y) const;
    bool pinContains(int x, int y) const;
    bool minimizeContains(int x, int y) const;
    bool disconnectContains(int x, int y) const;
    int toolbarLeft() const;
    int sliderLeft() const;
    int sliderRight() const;

    SDL_Window* m_Window;
    Overlay::OverlayManager& m_OverlayManager;
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
    int m_Width;
    int m_ToolbarLeft;
    int m_ToolbarDragOffsetX;
    int m_PointerX;
    int m_PointerY;
    int m_BitrateKbps;
    int m_LastSentBitrateKbps;
    int m_AppliedBitrateKbps;
    int m_AppliedPeakKbps;
    float m_RenderedFps;
    float m_VideoMbps;
    float m_LastDrawnFps;
    float m_LastDrawnVideoMbps;
    Uint32 m_HideDeadline;
    Uint32 m_LastBitrateSendTime;
    Uint32 m_LastBitrateChangeTime;
    Uint32 m_LastToolbarMoveDrawTime;
    Uint32 m_LastRedrawTime;
    Uint32 m_EdgeHoverStartTime;
};
