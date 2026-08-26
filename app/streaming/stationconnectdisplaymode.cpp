#include "stationconnectdisplaymode.h"

#include <QtMath>

QSize StationConnectDisplayMode::qualifiedMaximum()
{
    return QSize(3840, 2160);
}

QSize StationConnectDisplayMode::resolve(const QSize& detectedResolution,
                                         const QSize& hostCanvasResolution,
                                         const QSize& maximumResolution)
{
    const QSize maximum = maximumResolution.isValid() ?
                              maximumResolution : qualifiedMaximum();
    QSize destination = detectedResolution;
    if (destination.width() < 2 || destination.height() < 2) {
        destination = maximum;
    }

    // Scale the host canvas directly to the largest stream that fits the
    // physical client display. A fixed 3840-wide intermediate would make a
    // 5120x2160 host canvas become 3840x1620 and then be enlarged again on a
    // 4096x1728 client, adding a needless filtered downscale/upscale round
    // trip. Never upscale the host canvas here either; presentation performs
    // the one unavoidable enlargement when the client has more pixels.
    const QSize source = hostCanvasResolution.isValid() ?
                             hostCanvasResolution : destination;
    return fitWithin(source, destination);
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
