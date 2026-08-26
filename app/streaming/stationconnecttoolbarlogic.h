#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace StationConnectToolbarLogic {

inline float resolvePixelDensity(float reportedDensity,
                                 int logicalWidth,
                                 int logicalHeight,
                                 int pixelWidth,
                                 int pixelHeight)
{
    if (std::isfinite(reportedDensity) && reportedDensity > 0.0f) {
        return reportedDensity;
    }

    const float widthDensity = logicalWidth > 0 && pixelWidth > 0 ?
                static_cast<float>(pixelWidth) / logicalWidth : 0.0f;
    const float heightDensity = logicalHeight > 0 && pixelHeight > 0 ?
                static_cast<float>(pixelHeight) / logicalHeight : 0.0f;
    if (widthDensity > 0.0f && heightDensity > 0.0f) {
        return (widthDensity + heightDensity) * 0.5f;
    }
    if (widthDensity > 0.0f) {
        return widthDensity;
    }
    if (heightDensity > 0.0f) {
        return heightDensity;
    }
    return 1.0f;
}

inline int physicalExtent(int logicalExtent, float pixelDensity)
{
    return std::max(1, static_cast<int>(std::lround(
                          logicalExtent * std::max(pixelDensity, 0.01f))));
}

inline float horizontalPosition(int logicalLeft,
                                int logicalWindowWidth,
                                int logicalToolbarWidth)
{
    const int availableWidth = std::max(
                0, logicalWindowWidth - logicalToolbarWidth);
    return availableWidth > 0 ?
                std::clamp(static_cast<float>(logicalLeft) / availableWidth,
                           0.0f, 1.0f) : 0.5f;
}

inline int logicalLeftFromPosition(float position,
                                   int logicalWindowWidth,
                                   int logicalToolbarWidth)
{
    const int availableWidth = std::max(
                0, logicalWindowWidth - logicalToolbarWidth);
    return static_cast<int>(std::lround(
                std::clamp(position, 0.0f, 1.0f) * availableWidth));
}

// Native child-surface pointer events are already expressed in toolbar-local
// logical coordinates by the compositor. Only the child's parent-relative
// origin is added; pixel-density and streamed-video transforms never apply.
inline int nativePointerParentCoordinate(int childOrigin, int localCoordinate)
{
    return childOrigin + localCoordinate;
}

// Wayland intentionally does not expose a root-window pointer coordinate to a
// child surface. Keep the child fixed for the duration of the implicit button
// grab and derive the final parent-relative origin from the press-local and
// release-local coordinates. Moving the child while using its changing local
// coordinates creates a feedback loop and can clamp the toolbar to an edge.
inline int nativeDragFinalLeft(int startLeft,
                               int pressLocalX,
                               int currentLocalX,
                               int logicalWindowWidth,
                               int logicalToolbarWidth)
{
    return std::clamp(startLeft + currentLocalX - pressLocalX,
                      0,
                      std::max(0, logicalWindowWidth - logicalToolbarWidth));
}

class ButtonRouter
{
public:
    enum class Owner {
        Local,
        Remote,
    };

    Owner routeButton(uint8_t button, bool down, bool pointerInside)
    {
        const uint32_t bit = button > 0 && button <= 32 ?
                    uint32_t(1) << (button - 1) : 0;

        if (!down) {
            if ((m_LocalButtons & bit) != 0) {
                m_LocalButtons &= ~bit;
                return Owner::Local;
            }
            if ((m_RemoteButtons & bit) != 0) {
                m_RemoteButtons &= ~bit;
                return Owner::Remote;
            }
        }

        const Owner owner = m_LocalButtons != 0 ? Owner::Local :
                            m_RemoteButtons != 0 ? Owner::Remote :
                            pointerInside ? Owner::Local : Owner::Remote;
        if (down && bit != 0) {
            if (owner == Owner::Local) {
                m_LocalButtons |= bit;
            } else {
                m_RemoteButtons |= bit;
            }
        }
        return owner;
    }

    Owner routeMotion(bool pointerInside) const
    {
        return m_LocalButtons != 0 ? Owner::Local :
               m_RemoteButtons != 0 ? Owner::Remote :
               pointerInside ? Owner::Local : Owner::Remote;
    }

    bool hasLocalButtons() const { return m_LocalButtons != 0; }
    bool hasRemoteButtons() const { return m_RemoteButtons != 0; }

    void reset()
    {
        m_LocalButtons = 0;
        m_RemoteButtons = 0;
    }

private:
    uint32_t m_LocalButtons = 0;
    uint32_t m_RemoteButtons = 0;
};

}
