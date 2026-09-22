#include "plankpresentation.h"

#include <QtMath>

#include <cmath>

QRect PlankPresentation::aspectFitRect(const QSize& sourceSize,
                                        const QRect& destination)
{
    if (sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
            destination.width() <= 0 || destination.height() <= 0) {
        return QRect();
    }

    // Keep this arithmetic (single-precision, ceil, integer halving) exactly
    // as the renderers have always letterboxed: the Metal, SDL and D3D/VA
    // renderers reach it through StreamUtils::scaleSourceToDestinationSurface.
    const int fittedHeight = static_cast<int>(std::ceil(
        static_cast<float>(destination.width()) * sourceSize.height() /
            sourceSize.width()));
    const int fittedWidth = static_cast<int>(std::ceil(
        static_cast<float>(destination.height()) * sourceSize.width() /
            sourceSize.height()));

    if (fittedHeight > destination.height()) {
        return QRect(destination.x() + (destination.width() - fittedWidth) / 2,
                     destination.y(), fittedWidth, destination.height());
    }
    return QRect(destination.x(),
                 destination.y() + (destination.height() - fittedHeight) / 2,
                 destination.width(), fittedHeight);
}

QRect PlankPresentation::videoRect(const QSize& streamSize,
                                   const QSize& canvasSize)
{
    if (!streamSize.isValid() || !canvasSize.isValid()) {
        return QRect();
    }
    return aspectFitRect(streamSize, QRect(QPoint(0, 0), canvasSize));
}

PlankOutputGeometry PlankPresentation::outputGeometry(
        const PlankPresentationLayout& layout,
        const PlankPresentationOutput& output,
        const QSize& windowSize,
        const QSize& drawableSize,
        bool liveDrawable)
{
    PlankOutputGeometry geometry;
    geometry.windowSize = windowSize;
    if (liveDrawable && !layout.isMultiOutput() &&
            drawableSize.width() > 0 && drawableSize.height() > 0) {
        geometry.canvasSize = drawableSize;
        geometry.canvasRect = QRect(QPoint(0, 0), drawableSize);
        geometry.live = true;
    }
    else {
        geometry.canvasSize = layout.canvasSize;
        geometry.canvasRect = output.canvasRect;
    }
    return geometry;
}

QRectF PlankPresentation::videoRectInWindow(
        const QSize& streamSize, const PlankOutputGeometry& geometry)
{
    if (!geometry.isValid() || !streamSize.isValid() ||
            streamSize.isEmpty()) {
        return QRectF();
    }
    const QRect visible = videoRect(streamSize, geometry.canvasSize)
            .intersected(geometry.canvasRect);
    if (visible.isEmpty()) {
        return QRectF();
    }
    const qreal scaleX = static_cast<qreal>(geometry.windowSize.width()) /
            geometry.canvasRect.width();
    const qreal scaleY = static_cast<qreal>(geometry.windowSize.height()) /
            geometry.canvasRect.height();
    return QRectF((visible.left() - geometry.canvasRect.left()) * scaleX,
                  (visible.top() - geometry.canvasRect.top()) * scaleY,
                  visible.width() * scaleX,
                  visible.height() * scaleY);
}

bool PlankPresentation::mapWindowPointToStream(
        const QPointF& windowPoint,
        const PlankOutputGeometry& geometry,
        const QSize& streamSize,
        QPointF& streamPoint,
        bool allowClampedPosition)
{
    return mapWindowPointToStream(windowPoint, geometry.windowSize,
                                  streamSize, geometry.canvasSize,
                                  geometry.canvasRect, streamPoint,
                                  allowClampedPosition);
}

bool PlankPresentation::mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const PlankOutputGeometry& geometry,
        QPointF& windowPoint)
{
    return mapStreamPointToWindow(streamPoint, streamSize,
                                  geometry.canvasSize, geometry.canvasRect,
                                  geometry.windowSize, windowPoint);
}

QPoint PlankPresentation::absoluteStreamPosition(const QPointF& streamPoint,
                                                 const QSize& streamSize)
{
    return QPoint(qBound(0, qRound(streamPoint.x()),
                         qMax(0, streamSize.width() - 1)),
                  qBound(0, qRound(streamPoint.y()),
                         qMax(0, streamSize.height() - 1)));
}

PlankPresentationSlice PlankPresentation::sliceForOutput(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect)
{
    PlankPresentationSlice slice;
    const QRect destination = videoRect(streamSize, canvasSize);
    const QRect visible = destination.intersected(outputCanvasRect);
    if (visible.isEmpty()) {
        return slice;
    }

    const qreal sourceScaleX = static_cast<qreal>(streamSize.width()) /
            destination.width();
    const qreal sourceScaleY = static_cast<qreal>(streamSize.height()) /
            destination.height();
    slice.sourceRect = QRectF(
        (visible.left() - destination.left()) * sourceScaleX,
        (visible.top() - destination.top()) * sourceScaleY,
        visible.width() * sourceScaleX,
        visible.height() * sourceScaleY);
    slice.destinationRect = visible.translated(-outputCanvasRect.topLeft());
    slice.visible = true;
    return slice;
}

bool PlankPresentation::mapWindowPointToStream(
        const QPointF& windowPoint,
        const QSize& windowSize,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        QPointF& streamPoint,
        bool allowClampedPosition)
{
    if (windowSize.isEmpty() || streamSize.isEmpty() ||
            canvasSize.isEmpty() || !outputCanvasRect.isValid()) {
        return false;
    }

    const QPointF canvasPoint(
        outputCanvasRect.left() +
            windowPoint.x() * outputCanvasRect.width() / windowSize.width(),
        outputCanvasRect.top() +
            windowPoint.y() * outputCanvasRect.height() / windowSize.height());
    const QRect destination = videoRect(streamSize, canvasSize);
    if (destination.isEmpty()) {
        return false;
    }
    const bool inside = destination.contains(qFloor(canvasPoint.x()),
                                             qFloor(canvasPoint.y()));
    if (!inside && !allowClampedPosition) {
        return false;
    }

    const qreal x = qBound<qreal>(0.0,
        canvasPoint.x() - destination.left(), destination.width());
    const qreal y = qBound<qreal>(0.0,
        canvasPoint.y() - destination.top(), destination.height());
    streamPoint = QPointF(x * streamSize.width() / destination.width(),
                          y * streamSize.height() / destination.height());
    return true;
}

bool PlankPresentation::mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& windowSize,
        QPointF& windowPoint)
{
    if (windowSize.isEmpty() || streamSize.isEmpty() ||
            canvasSize.isEmpty() || !outputCanvasRect.isValid()) {
        return false;
    }

    const QRect destination = videoRect(streamSize, canvasSize);
    if (destination.isEmpty()) {
        return false;
    }
    const QPointF canvasPoint(
        destination.left() + streamPoint.x() * destination.width() /
            streamSize.width(),
        destination.top() + streamPoint.y() * destination.height() /
            streamSize.height());
    if (!outputCanvasRect.contains(qFloor(canvasPoint.x()),
                                   qFloor(canvasPoint.y()))) {
        return false;
    }

    windowPoint = QPointF(
        (canvasPoint.x() - outputCanvasRect.left()) * windowSize.width() /
            outputCanvasRect.width(),
        (canvasPoint.y() - outputCanvasRect.top()) * windowSize.height() /
            outputCanvasRect.height());
    return true;
}

QRectF PlankPresentation::sourceRectInStream(const QRect& captureRect,
                                             const QSize& captureSize,
                                             const QSize& streamSize)
{
    if (!captureRect.isValid() || captureSize.isEmpty() || streamSize.isEmpty()) {
        return QRectF();
    }
    const qreal scaleX = static_cast<qreal>(streamSize.width()) / captureSize.width();
    const qreal scaleY = static_cast<qreal>(streamSize.height()) / captureSize.height();
    return QRectF(captureRect.x() * scaleX, captureRect.y() * scaleY,
                  captureRect.width() * scaleX, captureRect.height() * scaleY);
}

PlankPresentationSlice PlankPresentation::sliceForSource(const QSize& streamSize,
                                                         const QRectF& sourceRect,
                                                         const QSize& drawableSize)
{
    PlankPresentationSlice slice;
    if (streamSize.isEmpty() || drawableSize.isEmpty() || !sourceRect.isValid()) {
        return slice;
    }
    const QRectF source = sourceRect.intersected(QRectF(QPointF(0, 0), QSizeF(streamSize)));
    if (source.isEmpty()) {
        return slice;
    }
    const QRect destination = aspectFitRect(QSize(qMax(1, qRound(source.width())),
                                                  qMax(1, qRound(source.height()))),
                                            QRect(QPoint(0, 0), drawableSize));
    if (destination.isEmpty()) {
        return slice;
    }
    slice.sourceRect = source;
    slice.destinationRect = destination;
    slice.visible = true;
    return slice;
}

bool PlankPresentation::mapWindowPointToDesktop(const QPointF& windowPoint,
                                                const QSize& windowSize,
                                                const QSize& drawableSize,
                                                const PlankPresentationOutput& output,
                                                const QSize& streamSize,
                                                QPointF& desktopPoint,
                                                bool allowClampedPosition)
{
    if (windowSize.isEmpty() || !output.desktopRect.isValid() || output.desktopRect.isEmpty()) {
        return false;
    }
    const QSize drawable = drawableSize.isEmpty() ? windowSize : drawableSize;
    const PlankPresentationSlice slice = sliceForSource(streamSize, output.sourceRect, drawable);
    if (!slice.visible) {
        return false;
    }
    const QPointF drawablePoint(windowPoint.x() * drawable.width() / windowSize.width(),
                                windowPoint.y() * drawable.height() / windowSize.height());
    const QRect& video = slice.destinationRect;
    const bool inside = video.contains(qFloor(drawablePoint.x()), qFloor(drawablePoint.y()));
    if (!inside && !allowClampedPosition) {
        return false;
    }
    const qreal fractionX = qBound<qreal>(0.0, (drawablePoint.x() - video.left()) / video.width(), 1.0);
    const qreal fractionY = qBound<qreal>(0.0, (drawablePoint.y() - video.top()) / video.height(), 1.0);
    desktopPoint = QPointF(output.desktopRect.left() + fractionX * output.desktopRect.width(),
                           output.desktopRect.top() + fractionY * output.desktopRect.height());
    return true;
}

bool PlankPresentation::mapStreamPointToSourceWindow(const QPointF& streamPoint,
                                                     const QSize& streamSize,
                                                     const PlankPresentationOutput& output,
                                                     const QSize& windowSize,
                                                     const QSize& drawableSize,
                                                     QPointF& windowPoint)
{
    if (windowSize.isEmpty()) {
        return false;
    }
    const QSize drawable = drawableSize.isEmpty() ? windowSize : drawableSize;
    const PlankPresentationSlice slice = sliceForSource(streamSize, output.sourceRect, drawable);
    if (!slice.visible || streamPoint.x() < slice.sourceRect.left() || streamPoint.y() < slice.sourceRect.top() ||
            streamPoint.x() >= slice.sourceRect.right() || streamPoint.y() >= slice.sourceRect.bottom()) {
        return false;
    }
    const QRect& video = slice.destinationRect;
    const QPointF drawablePoint(
        video.left() + (streamPoint.x() - slice.sourceRect.left()) * video.width() / slice.sourceRect.width(),
        video.top() + (streamPoint.y() - slice.sourceRect.top()) * video.height() / slice.sourceRect.height());
    windowPoint = QPointF(drawablePoint.x() * windowSize.width() / drawable.width(),
                          drawablePoint.y() * windowSize.height() / drawable.height());
    return true;
}
