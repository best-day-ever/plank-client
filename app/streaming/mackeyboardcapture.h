#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>

// Main-thread, session-scoped keyboard capture. Reserved local controls run
// before the remote queue; ordinary input is delivered by session dispatch.
class MacKeyboardCapture
{
public:
    // Call from the launcher/settings UI, never while a stream owns input.
    static void requestPermissionIfNeeded(bool captureEnabled);

    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys);
    ~MacKeyboardCapture();
    void refresh();
    void setSessionHotkeyHandler(std::function<bool(const SDL_KeyboardEvent&)> handler);
    bool dispatch(const SDL_Event& event,
                  const std::function<void(SDL_KeyboardEvent*)>& deliver);
    bool suppressSdlKeyEvent() const;
    // Qt owns its event loop while streaming. Reuse the bounded native tap
    // queue, but wake Qt directly rather than polling SDL's event queue.
    void setQueueWake(std::function<bool()> wake);
    void dispatchPending(const std::function<void(SDL_KeyboardEvent*)>& deliver);

    MacKeyboardCapture(const MacKeyboardCapture&) = delete;
    MacKeyboardCapture& operator=(const MacKeyboardCapture&) = delete;

private:
    friend class TestMacKeyboardCapture;
    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys, bool monitor);
    struct State;
    std::unique_ptr<State> m_State;
};
