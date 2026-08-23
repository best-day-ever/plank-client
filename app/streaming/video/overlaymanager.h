#pragma once

#include <QString>

#include <SDL.h>
#include <SDL_ttf.h>

#include <atomic>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayToolbar,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);
    void updateOverlaySurface(OverlayType type, SDL_Surface* surface);
    void setOverlayHorizontalPosition(OverlayType type, float position);
    float getOverlayHorizontalPosition(OverlayType type) const;

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[512];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    std::atomic<float> m_HorizontalPositions[OverlayMax];
    IOverlayRenderer* m_Renderer;
    QByteArray m_FontData;
};

}
