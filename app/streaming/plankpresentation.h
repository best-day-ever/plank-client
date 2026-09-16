#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>

#include <SDL3/SDL.h>

struct PlankPresentationOutput
{
    SDL_Window* window = nullptr;
    QRect canvasRect;
    bool primary = false;
};

struct PlankPresentationLayout
{
    QSize canvasSize;
    QVector<PlankPresentationOutput> outputs;

    bool isMultiOutput() const
    {
        return outputs.size() > 1;
    }
};

struct PlankPresentationSlice
{
    QRectF sourceRect;
    QRect destinationRect;
    bool visible = false;
};

class PlankPresentation
{
public:
    static QRect videoRect(const QSize& streamSize, const QSize& canvasSize);

    static PlankPresentationSlice sliceForOutput(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect);

    // Convert the shared canvas slice to an individual window's backing pixels.
    // Input uses logical window coordinates against the same canvas rectangle.
    static PlankPresentationSlice sliceForDrawable(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& drawableSize);

    static bool mapWindowPointToStream(
        const QPointF& windowPoint,
        const QSize& windowSize,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        QPointF& streamPoint,
        bool allowClampedPosition);

    static bool mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& windowSize,
        QPointF& windowPoint);
};
