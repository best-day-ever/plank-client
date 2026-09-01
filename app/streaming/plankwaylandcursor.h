#pragma once

#include <QImage>
#include <SDL3/SDL.h>

#include <memory>

class PlankWaylandCursor
{
public:
    static std::unique_ptr<PlankWaylandCursor> create(
            SDL_Window* parentWindow);
    ~PlankWaylandCursor();

    void dispatchPending();
    bool isAttachedTo(SDL_Window* parentWindow) const;
    void setImage(const QImage& image, int hotspotX, int hotspotY);
    void setPosition(int hotspotX, int hotspotY);
    void setVisible(bool visible);

private:
    class Impl;

    explicit PlankWaylandCursor(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_Impl;
};
