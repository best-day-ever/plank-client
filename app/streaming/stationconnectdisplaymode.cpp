#include "stationconnectdisplaymode.h"

#include <QtMath>

QSize StationConnectDisplayMode::qualifiedMaximum()
{
    return QSize(3840, 2160);
}

QSize StationConnectDisplayMode::resolve(bool autoResolution,
                                         const QSize& detectedResolution,
                                         const QSize& configuredResolution,
                                         const QSize& exactNativeResolution,
                                         const QSize& maximumResolution)
{
    const QSize maximum = maximumResolution.isValid() ?
                              maximumResolution : qualifiedMaximum();
    QSize requested = autoResolution ? detectedResolution : configuredResolution;
    if (requested.width() < 2 || requested.height() < 2) {
        requested = maximum;
    }

    // Avoid a lossy downscale/upscale round trip when the selected host
    // canvas already matches the physical client display pixel-for-pixel.
    if (autoResolution && exactNativeResolution.isValid() &&
            detectedResolution == exactNativeResolution) {
        return fitWithin(requested, requested);
    }

    return fitWithin(requested, maximum);
}

QSize StationConnectDisplayMode::fitWithin(const QSize& requestedResolution,
                                            const QSize& maximumResolution)
{
    const qreal widthScale = static_cast<qreal>(maximumResolution.width()) /
                             requestedResolution.width();
    const qreal heightScale = static_cast<qreal>(maximumResolution.height()) /
                              requestedResolution.height();
    const qreal scale = qMin<qreal>(1.0, qMin(widthScale, heightScale));

    // H.264 4:4:4 accepts arbitrary dimensions, but keeping both axes even
    // avoids one-pixel padding differences in renderers and host scalers.
    const int width = qMax(2, qFloor(requestedResolution.width() * scale) & ~1);
    const int height = qMax(2, qFloor(requestedResolution.height() * scale) & ~1);
    return QSize(width, height);
}
