#pragma once

#include <SDL3/SDL.h>

// An embedded video cursor must be the only cursor over remote pixels. SDL's
// Wayland tablet ShowCursor path can use cur_cursor even when asked to hide.
// A transparent image also hides that tool without a second Wayland connection,
// pointer warps, device grabs, or modifications to the Linux remote-cursor path.
class PlankEmbeddedCursor
{
public:
    ~PlankEmbeddedCursor()
    {
        if (m_Blank != nullptr) {
            if (SDL_GetCursor() == m_Blank) {
                SDL_SetCursor(SDL_GetDefaultCursor());
                SDL_ShowCursor();
            }
            SDL_DestroyCursor(m_Blank);
        }
    }

    bool setVisible(bool visible)
    {
        if (visible) {
            return m_Blank == nullptr || SDL_GetCursor() != m_Blank ||
                    SDL_SetCursor(SDL_GetDefaultCursor());
        }
        if (m_Blank == nullptr) {
            SDL_Surface* surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA8888);
            if (surface == nullptr) {
                return false;
            }
            if (SDL_FillSurfaceRect(surface, nullptr, 0)) {
                m_Blank = SDL_CreateColorCursor(surface, 0, 0);
            }
            SDL_DestroySurface(surface);
            if (m_Blank == nullptr) {
                return false;
            }
        }
        return SDL_GetCursor() == m_Blank || SDL_SetCursor(m_Blank);
    }

    PlankEmbeddedCursor() = default;
    PlankEmbeddedCursor(const PlankEmbeddedCursor&) = delete;
    PlankEmbeddedCursor& operator=(const PlankEmbeddedCursor&) = delete;

private:
    SDL_Cursor* m_Blank = nullptr;
};
