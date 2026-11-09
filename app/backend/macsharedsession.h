#pragma once

#include "outputtopology.h"
#include "settings/streamingpreferences.h"

namespace MacSharedSession {
// Called only after the full fixed-capture topology parser succeeds. A shared
// desktop keeps its current geometry/profile while another viewer is attached.
// Exclusive hosts must still satisfy the exact bookmark request.
inline bool acceptsDisplay(const NvOutputTopology& topology, const QSize& requested,
                           int scale, const QString& encodingMode, bool shared)
{
    if ((scale != 1 && scale != 2) || topology.featureFlags != NvOutputTopology::FixedCaptureFlags ||
        topology.desktopWidth <= 0 || topology.desktopHeight <= 0) return false;
    if (shared) return true;
    return topology.desktopWidth == requested.width() && topology.desktopHeight == requested.height() &&
        topology.captureLogicalBounds.size() == QSizeF(requested.width() / scale, requested.height() / scale) &&
        topology.appleEncodingMode == encodingMode;
}

// Session-only selection: the saved bookmark is never overwritten by another
// viewer's active desktop. The decoder must prove this exact returned profile.
inline int videoProfile(const NvOutputTopology& topology, int configured, bool shared)
{
    if (!shared || topology.featureFlags != NvOutputTopology::FixedCaptureFlags) return configured;
    if (topology.appleEncodingMode == QLatin1String("hevc-10-420-videotoolbox"))
        return StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420;
    if (topology.appleEncodingMode == QLatin1String("hevc-10-444-videotoolbox"))
        return StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444;
    return StreamingPreferences::PLANK_PROFILE_COUNT;
}
}
