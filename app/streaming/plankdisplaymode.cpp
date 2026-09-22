#include "plankdisplaymode.h"

#include <QtMath>

QSize PlankDisplayMode::qualifiedMaximum()
{
    return QSize(3840, 2160);
}

QSize PlankDisplayMode::resolveClient(const QSize& canvas, const QSize& limit, QString* error)
{
    if (error != nullptr) error->clear();
    if (!canvas.isValid() || canvas.width() < 2 || canvas.height() < 2 || (canvas.width() & 1) ||
            (canvas.height() & 1)) {
        if (error != nullptr) {
            *error = QStringLiteral("The workstation desktop size %1x%2 cannot be streamed.")
                    .arg(canvas.width()).arg(canvas.height());
        }
        return QSize();
    }
    if (limit.isValid() && (canvas.width() > limit.width() || canvas.height() > limit.height())) {
        if (error != nullptr) {
            *error = QStringLiteral("The workstation desktop (%1x%2) is larger than this stream can carry (%3x%4).")
                    .arg(canvas.width()).arg(canvas.height()).arg(limit.width()).arg(limit.height());
        }
        return QSize();
    }
    return canvas;
}

QSize PlankDisplayMode::resolve(const QSize& detectedResolution,
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

QSize PlankDisplayMode::fitWithin(const QSize& requestedResolution,
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
