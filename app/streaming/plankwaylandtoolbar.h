#pragma once

#include <QImage>
#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>

class PlankWaylandToolbar
{
public:
    struct Callbacks {
        std::function<void(int x, int y)> enter;
        std::function<void()> leave;
        std::function<void(int x, int y)> motion;
        std::function<void(uint32_t button, bool down)> button;
        std::function<void(int verticalSteps)> wheel;
    };

    static std::unique_ptr<PlankWaylandToolbar> create(
            SDL_Window* parentWindow, Callbacks callbacks);
    ~PlankWaylandToolbar();

    void dispatchPending();
    bool isAttachedTo(SDL_Window* parentWindow) const;
    void setLayout(int parentWidth, int toolbarX,
                   int toolbarWidth, int toolbarHeight);
    void setLayoutAt(int parentWidth, int surfaceY, int contentX,
                     int contentWidth, int contentHeight);
    void setVisible(bool visible);
    void present(const QImage& image);

private:
    class Impl;

    explicit PlankWaylandToolbar(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_Impl;
};
