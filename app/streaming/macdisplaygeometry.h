#pragma once

#include <algorithm>
#include <cmath>

namespace MacDisplayGeometry {
// Each presentation window uses native fullscreen, including mixed-DPI
// multi-display sessions. Match Client must use the same camera-safe viewport.
// Set SDL's Spaces hint before video initialization, when Cocoa caches it.
inline bool useNativeFullscreen(int connectedDisplayCount)
{
    return connectedDisplayCount > 0;
}

// A notched screen's fullscreen top exclusion can extend below its camera
// safe area. Use AppKit's measured top-edge reservation, not an OS-version
// margin. visibleFrame's bottom/side exclusions (the Dock) do not participate.
// Non-notched fullscreen windows still use the complete panel, even when the
// desktop's visibleFrame reserves a menu bar.
inline int nativeFullscreenTopInset(double cameraInset, double reservedTop)
{
    if (!std::isfinite(cameraInset) || cameraInset <= 0) return 0;
    if (!std::isfinite(reservedTop)) reservedTop = 0;
    return static_cast<int>(std::ceil(std::max(cameraInset, reservedTop)));
}

// Match the native fullscreen viewport without changing Retina density.
// AppKit supplies the inset; zero means the complete panel remains usable.
inline bool insetTop(int logicalWidth, int& logicalHeight,
                     int pixelWidth, int& pixelHeight, int top)
{
    if (logicalWidth <= 0 || logicalHeight <= 0 || top < 0 || top >= logicalHeight)
        return false;
    const int scale = pixelWidth / logicalWidth;
    if ((scale != 1 && scale != 2) || pixelWidth != logicalWidth * scale ||
            pixelHeight != logicalHeight * scale)
        return false;
    logicalHeight -= top;
    pixelHeight = logicalHeight * scale;
    return true;
}
}
