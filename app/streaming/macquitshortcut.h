#pragma once

#include <functional>
#include <memory>

// Main-thread, session-scoped ownership of the native Quit key equivalent.
// The menu action and the application Quit/cleanup bridge remain untouched.
class MacQuitShortcut
{
public:
    explicit MacQuitShortcut(std::function<bool()> remoteOwnsKeyboard);
    ~MacQuitShortcut();
    void refresh();

    MacQuitShortcut(const MacQuitShortcut&) = delete;
    MacQuitShortcut& operator=(const MacQuitShortcut&) = delete;

private:
    struct State;
    std::unique_ptr<State> m_State;
};
