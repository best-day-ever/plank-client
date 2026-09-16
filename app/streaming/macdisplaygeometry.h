#pragma once

namespace MacDisplayGeometry {
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
