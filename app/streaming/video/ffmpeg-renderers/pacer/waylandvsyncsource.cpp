#include "waylandvsyncsource.h"

#include <SDL3/SDL_system.h>

#ifndef SDL_VIDEO_DRIVER_WAYLAND
#warning Unable to use WaylandVsyncSource without SDL support
#else

const struct wl_callback_listener WaylandVsyncSource::s_FrameListener = {
    .done = WaylandVsyncSource::frameDone,
};

WaylandVsyncSource::WaylandVsyncSource(Pacer* pacer)
    : m_Pacer(pacer),
      m_Display(nullptr),
      m_Surface(nullptr),
      m_Callback(nullptr)
{

}

WaylandVsyncSource::~WaylandVsyncSource()
{
    if (m_Callback != nullptr) {
        wl_callback_destroy(m_Callback);
        wl_display_roundtrip(m_Display);
    }
}

bool WaylandVsyncSource::initialize(SDL_Window* window, int)
{
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
    if (properties == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetWindowProperties() failed: %s",
                     SDL_GetError());
        return false;
    }

    m_Display = static_cast<wl_display*>(SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    m_Surface = static_cast<wl_surface*>(SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (m_Display == nullptr || m_Surface == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL Wayland window properties are unavailable");
        return false;
    }

    // Enqueue our first frame callback
    m_Callback = wl_surface_frame(m_Surface);
    wl_callback_add_listener(m_Callback, &s_FrameListener, this);
    wl_surface_commit(m_Surface);

    return true;
}

bool WaylandVsyncSource::isAsync()
{
    // Wayland frame callbacks are asynchronous
    return true;
}

void WaylandVsyncSource::frameDone(void* data, struct wl_callback* oldCb, uint32_t)
{
    auto me = (WaylandVsyncSource*)data;

    // Free this callback
    SDL_assert(oldCb == me->m_Callback);
    wl_callback_destroy(oldCb);

    // Wake the Pacer Vsync thread
    me->m_Pacer->signalVsync();

    // Register for another callback
    me->m_Callback = wl_surface_frame(me->m_Surface);
    wl_callback_add_listener(me->m_Callback, &s_FrameListener, data);
    wl_surface_commit(me->m_Surface);
    wl_display_flush(me->m_Display);
}

#endif
