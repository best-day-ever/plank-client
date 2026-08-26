#pragma once

#include <QImage>
#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>

class StationConnectWaylandToolbar
{
public:
    struct Callbacks {
        std::function<void(int x, int y)> enter;
        std::function<void()> leave;
        std::function<void(int x, int y)> motion;
        std::function<void(uint32_t button, bool down)> button;
        std::function<void(int verticalSteps)> wheel;
    };

    static std::unique_ptr<StationConnectWaylandToolbar> create(
            SDL_Window* parentWindow, Callbacks callbacks);
    ~StationConnectWaylandToolbar();

    void dispatchPending();
    bool isAttachedTo(SDL_Window* parentWindow) const;
    void setGeometry(int x, int y, int width, int height);
    void setVisible(bool visible);
    void present(const QImage& image);

private:
    class Impl;

    explicit StationConnectWaylandToolbar(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_Impl;
};
