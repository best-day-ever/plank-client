#pragma once

#include <SDL.h>

class StreamingPreferences;

namespace Overlay {
class OverlayManager;
}

class StationConnectToolbar
{
public:
    enum class Action {
        None,
        Consumed,
        Disconnect,
    };

    StationConnectToolbar(SDL_Window* window,
                          Overlay::OverlayManager& overlayManager,
                          StreamingPreferences& preferences);
    ~StationConnectToolbar();

    void setRenderedFps(float fps);
    void update(Uint32 now);
    void notifyWindowChanged();

    bool handleMouseMotion(const SDL_MouseMotionEvent& event);
    Action handleMouseButton(const SDL_MouseButtonEvent& event);
    bool handleMouseWheel(const SDL_MouseWheelEvent& event);

    int eventWaitTimeout() const;

private:
    void show(Uint32 now);
    void hide();
    void redraw();
    void updateBitrateFromPointer(int x, Uint32 now, bool forceSend);
    void queueBitrateRequest(Uint32 now, bool forceSend);
    bool contains(int x, int y) const;
    bool sliderContains(int x, int y) const;
    bool pinContains(int x, int y) const;
    bool disconnectContains(int x, int y) const;
    int toolbarLeft() const;
    int sliderLeft() const;
    int sliderRight() const;

    SDL_Window* m_Window;
    Overlay::OverlayManager& m_OverlayManager;
    StreamingPreferences& m_Preferences;
    bool m_Visible;
    bool m_Pinned;
    bool m_DraggingSlider;
    bool m_PointerInside;
    bool m_PointerInitialized;
    bool m_BitrateSupported;
    int m_WindowWidth;
    int m_WindowHeight;
    int m_Width;
    int m_PointerX;
    int m_PointerY;
    int m_BitrateKbps;
    int m_LastSentBitrateKbps;
    float m_RenderedFps;
    float m_LastDrawnFps;
    Uint32 m_HideDeadline;
    Uint32 m_LastBitrateSendTime;
    Uint32 m_LastRedrawTime;
};
