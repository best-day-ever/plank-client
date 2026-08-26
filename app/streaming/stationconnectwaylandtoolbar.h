#pragma once

#include <QImage>
#include <SDL3/SDL.h>

#include <memory>

class StationConnectWaylandToolbar
{
public:
    static std::unique_ptr<StationConnectWaylandToolbar> create(
            SDL_Window* parentWindow);
    ~StationConnectWaylandToolbar();

    void dispatchPending();
    bool isAttachedTo(SDL_Window* parentWindow) const;
    SDL_WindowID windowId() const;
    void setLayout(int parentWidth, int toolbarX,
                   int toolbarWidth, int toolbarHeight);
    void setVisible(bool visible);
    void present(const QImage& image);

private:
    class Impl;

    explicit StationConnectWaylandToolbar(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_Impl;
};
