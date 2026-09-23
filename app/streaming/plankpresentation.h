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
    PlankPresentationOutput() = default;
    PlankPresentationOutput(SDL_Window* outputWindow, const QRect& outputCanvasRect, bool isPrimary)
        : window(outputWindow), canvasRect(outputCanvasRect), primary(isPrimary)
    {
    }

    SDL_Window* window = nullptr;
    QRect canvasRect;
    bool primary = false;
    // When valid, this window shows exactly this rectangle of the stream
    // (stream pixels: the host's capture_rect, else source_rect, scaled to
    // the stream), fitted into its own drawable, and pointer input maps
    // through it to desktopRect (host desktop coordinates, the host's output
    // rectangle).
    // Otherwise the window shows its canvasRect slice of the canvas.
    QRectF sourceRect;
    QRect desktopRect;
};

struct PlankPresentationLayout
{
    QSize canvasSize;
    QVector<PlankPresentationOutput> outputs;
    // Host desktop bounding box: the reference size for absolute pointer
    // positions when the outputs carry source rectangles.
    QSize desktopSize;

    bool isMultiOutput() const
    {
        return outputs.size() > 1;
    }

    bool usesSourceRects() const
    {
        if (outputs.isEmpty() || !desktopSize.isValid() || desktopSize.isEmpty()) {
            return false;
        }
        for (const PlankPresentationOutput& output : outputs) {
            if (!output.sourceRect.isValid() || output.sourceRect.isEmpty() ||
                    !output.desktopRect.isValid() || output.desktopRect.isEmpty()) {
                return false;
            }
        }
        return true;
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

// Decides when the decoder reports a decoded frame size to input.
//
// Input must map against the size the renderer letterboxes. Some renderers
// fit the decoded frame (frame->width/height); others fit the negotiated
// stream size and stretch the frame into it. Only the first kind may move
// input to the decoded size. Reports happen on a change only; reset() makes
// the next frame report again (for example after a reconnect, where the
// session re-applies the negotiated size to input while a retained decoder
// keeps decoding frames of the same size).
class PlankDecodedFrameSizeTracker
{
public:
    bool shouldReport(int width, int height, bool rendererFitsDecodedFrames)
    {
        if (!rendererFitsDecodedFrames || width <= 0 || height <= 0 ||
                (width == m_Width && height == m_Height)) {
            return false;
        }
        m_Width = width;
        m_Height = height;
        return true;
    }

    void reset()
    {
        m_Width = 0;
        m_Height = 0;
    }

private:
    int m_Width = 0;
    int m_Height = 0;
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
    // Captured drags stay relative to the window where the button went
    // down, even after crossing into another presentation window. Return
    // the window under that point and its local coordinates, or -1 outside
    // the presentation. Bounds and points are logical desktop coordinates.
    static int capturedPointerTarget(const QPointF& point, const QPoint& origin,
                                     const QVector<QRect>& windows, QPointF& localPoint);

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

    // ---- Outputs with a source rectangle (PlankPresentationOutput::sourceRect)

    // The host's capture rectangle of one output (capture_rect, in capture
    // pixels) as stream pixels: the capture may be scaled to the stream.
    static QRectF sourceRectInStream(const QRect& captureRect,
                                     const QSize& captureSize,
                                     const QSize& streamSize);

    // What one window draws: sourceRect of the stream (clipped to it),
    // aspect-fitted and centred in the window's drawable; the destination is
    // in drawable pixels.
    static PlankPresentationSlice sliceForSource(const QSize& streamSize,
                                                 const QRectF& sourceRect,
                                                 const QSize& drawableSize);

    // A window point (event coordinates) as a host desktop point, through
    // the window's drawable, its fitted source rectangle and its desktop
    // rectangle. Outside the video only with allowClampedPosition.
    static bool mapWindowPointToDesktop(const QPointF& windowPoint,
                                        const QSize& windowSize,
                                        const QSize& drawableSize,
                                        const PlankPresentationOutput& output,
                                        const QSize& streamSize,
                                        QPointF& desktopPoint,
                                        bool allowClampedPosition);

    // A stream point (the remote cursor) in this window, when the window
    // shows it.
    static bool mapStreamPointToSourceWindow(const QPointF& streamPoint,
                                             const QSize& streamSize,
                                             const PlankPresentationOutput& output,
                                             const QSize& windowSize,
                                             const QSize& drawableSize,
                                             QPointF& windowPoint);

    // Absolute pointer coordinates on the host desktop: rounded and kept
    // inside it (0..width-1, 0..height-1).
    static QPoint absoluteDesktopPosition(const QPointF& desktopPoint,
                                          const QSize& desktopSize)
    {
        return absoluteStreamPosition(desktopPoint, desktopSize);
    }
};
