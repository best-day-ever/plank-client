#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>

// Only a window handle is stored here; keep this pure geometry header free of
// SDL so its unit tests build without SDL.
struct SDL_Window;

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

// Geometry of one presentation window at the moment an input event is
// mapped. windowSize is in the window's event coordinates (SDL points);
// canvasSize/canvasRect are in the pixel space the renderer letterboxes into.
struct PlankOutputGeometry
{
    QSize windowSize;
    QSize canvasSize;
    QRect canvasRect;
    // True when canvasSize is the window's live drawable rather than the
    // presentation layout snapshot taken when the renderer was created.
    bool live = false;

    bool isValid() const
    {
        return windowSize.isValid() && !windowSize.isEmpty() &&
                canvasSize.isValid() && !canvasSize.isEmpty() &&
                canvasRect.isValid();
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
    // Aspect-fit source into destination and centre it. This is the one
    // letterbox computation shared by every renderer
    // (StreamUtils::scaleSourceToDestinationSurface delegates here) and by
    // input mapping, so input and video can never disagree by a rounding step.
    static QRect aspectFitRect(const QSize& sourceSize,
                               const QRect& destination);

    static QRect videoRect(const QSize& streamSize, const QSize& canvasSize);

    // Geometry for mapping input on one output window.
    //
    // A single-output presentation whose renderer letterboxes every frame
    // against the window's live drawable (liveDrawable) must be mapped against
    // that same live drawable: the layout snapshot is only taken at window
    // creation and fullscreen requests, and goes stale on every later resize,
    // asynchronous fullscreen transition or display-mode change. Multi-output
    // presentations and renderers that letterbox against the snapshot keep the
    // snapshot, so input keeps matching what those renderers draw.
    static PlankOutputGeometry outputGeometry(
        const PlankPresentationLayout& layout,
        const PlankPresentationOutput& output,
        const QSize& windowSize,
        const QSize& drawableSize,
        bool liveDrawable);

    // The video rectangle of this output, in window (event) coordinates.
    static QRectF videoRectInWindow(const QSize& streamSize,
                                    const PlankOutputGeometry& geometry);

    static bool mapWindowPointToStream(
        const QPointF& windowPoint,
        const PlankOutputGeometry& geometry,
        const QSize& streamSize,
        QPointF& streamPoint,
        bool allowClampedPosition);

    static bool mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const PlankOutputGeometry& geometry,
        QPointF& windowPoint);

    // Absolute pointer coordinates for the host: rounded and kept inside the
    // streamed image (0..width-1, 0..height-1).
    static QPoint absoluteStreamPosition(const QPointF& streamPoint,
                                         const QSize& streamSize);

    static PlankPresentationSlice sliceForOutput(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect);

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
