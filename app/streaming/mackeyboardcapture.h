#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>

// Main-thread, session-scoped keyboard capture. The tap only queues events;
// normal session dispatch performs PLANK hotkeys and remote input delivery.
class MacKeyboardCapture
{
public:
    // Call from the launcher/settings UI, never while a stream owns input.
    static void requestPermissionIfNeeded(bool captureEnabled);

    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys);
    ~MacKeyboardCapture();
    void refresh();
    bool dispatch(const SDL_Event& event,
                  const std::function<void(SDL_KeyboardEvent*)>& deliver);
    bool suppressSdlKeyEvent() const;

    MacKeyboardCapture(const MacKeyboardCapture&) = delete;
    MacKeyboardCapture& operator=(const MacKeyboardCapture&) = delete;

private:
    friend class TestMacKeyboardCapture;
    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys, bool monitor);
    struct State;
    std::unique_ptr<State> m_State;
};
