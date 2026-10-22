#pragma once

#include <SDL3/SDL.h>
#include <atomic>
#include <bitset>

// Reserved local controls. Deliberately independent of immersive mode,
// view-only access, clipboard handling and the remote keyboard event queue.
class PlankSessionHotkeys
{
public:
    enum Action : unsigned { None = 0, Disconnect = 1, ToggleToolbar = 2 };

    bool handle(const SDL_KeyboardEvent& key)
    {
        const auto scan = static_cast<unsigned>(key.scancode);
        const bool tracked = scan < SDL_SCANCODE_COUNT && m_ConsumedKeys[scan];
        if (!key.down) {
            if (tracked) m_ConsumedKeys.reset(scan);
            return tracked;
        }
        if (!(key.mod & SDL_KMOD_CTRL) || !(key.mod & SDL_KMOD_ALT) ||
                !(key.mod & SDL_KMOD_SHIFT)) return tracked;
        // Resolve layout-aware letters before the physical-key fallback.
        Action action = None;
        if (key.key == SDLK_Q) action = Disconnect;
        else if (key.key == SDLK_T) action = ToggleToolbar;
        else if (key.scancode == SDL_SCANCODE_Q) action = Disconnect;
        else if (key.scancode == SDL_SCANCODE_T) action = ToggleToolbar;
        if (action == None) return tracked;
        if (scan < SDL_SCANCODE_COUNT) m_ConsumedKeys.set(scan);
        if (!key.repeat) {
            if (action == Disconnect) m_Pending.fetch_or(Disconnect);
            else m_Pending.fetch_xor(ToggleToolbar);
        }
        return true;
    }

    Action take()
    {
        const unsigned pending = m_Pending.exchange(None);
        // Disconnect wins if both controls arrive in the same pump cycle.
        return (pending & Disconnect) ? Disconnect :
               (pending & ToggleToolbar) ? ToggleToolbar : None;
    }

    bool pending() const { return m_Pending.load() != None; }
    void releaseKeys() { m_ConsumedKeys.reset(); }

private:
    std::bitset<SDL_SCANCODE_COUNT> m_ConsumedKeys;
    std::atomic_uint m_Pending{None};
};
