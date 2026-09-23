#include <QtTest>

#include "streaming/plankpresentation.h"

class TestPlankPresentation : public QObject
{
    Q_OBJECT

private slots:
    void capturedDragCrossesBetweenDisplays();
    void capturedDragHonoursLogicalBoundsAndGaps();
    void exactDualOutputSlices();
    void letterboxedDualOutputSlices();
    void mapsEachWindowIntoOneStreamCanvas();
    void preservesMappingWithScaledLogicalWindows();
    void mapsCursorIntoSingleOutput();
    void mapsCursorAcrossDualOutputSeam();
    void mapsCursorAcrossAsymmetricOutputSeam();

    // Input geometry follows what the renderer draws (live drawable).
    void aspectFitMatchesRendererRounding();
    void fullscreenAfterStaleWindowedSnapshot();
    void windowedAfterStaleFullscreenSnapshot();
    void windowedStartMapsContentArea();
    void retinaWindowMapsThroughDrawable();
    void pillarboxAndFourByThree();
    void bestFitStreamOnRetinaPanel();
    void roundTripsAtScaleOneAndTwo();
    void clampsAbsolutePositionInsideImage();
    void snapshotRenderersAndMultiOutputKeepLayout();
    void videoRectInWindowPoints();
    void decodedSizeReportedOnlyForFrameFittingRenderers();
    void decodedSizeReportedAgainAfterReset();

    // Outputs with source rectangles (display arrangements).
    void sourceRectsSliceEachWindow();
    void sourceRectsFitMismatchedWindows();
    void packedCaptureMapsPointerToDesktop();
    void remoteCursorFindsItsWindow();
    void sourceRectsMatchLegacyExactLayout();
    void layoutRequiresCompleteSourceRects();
    void packedThreeUhdThirdScreenMapsToTheDesktop();

private:
    static PlankOutputGeometry singleOutput(const QSize& snapshotCanvas,
                                            const QSize& windowSize,
                                            const QSize& drawableSize,
                                            bool liveDrawable = true);
};

PlankOutputGeometry TestPlankPresentation::singleOutput(
        const QSize& snapshotCanvas, const QSize& windowSize,
        const QSize& drawableSize, bool liveDrawable)
{
    PlankPresentationLayout layout;
    layout.canvasSize = snapshotCanvas;
    layout.outputs.append({nullptr, QRect(QPoint(0, 0), snapshotCanvas), true});
    return PlankPresentation::outputGeometry(
                layout, layout.outputs.first(), windowSize, drawableSize,
                liveDrawable);
}

void TestPlankPresentation::exactDualOutputSlices()
{
    const QSize size(5120, 2160);
    const auto left = PlankPresentation::sliceForOutput(
                size, size, QRect(0, 0, 2560, 2160));
    const auto right = PlankPresentation::sliceForOutput(
                size, size, QRect(2560, 0, 2560, 2160));

    QVERIFY(left.visible);
    QCOMPARE(left.sourceRect, QRectF(0, 0, 2560, 2160));
    QCOMPARE(left.destinationRect, QRect(0, 0, 2560, 2160));
    QVERIFY(right.visible);
    QCOMPARE(right.sourceRect, QRectF(2560, 0, 2560, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 2560, 2160));
}

void TestPlankPresentation::letterboxedDualOutputSlices()
{
    const QSize stream(3840, 2160);
    const QSize canvas(5120, 2160);
    QCOMPARE(PlankPresentation::videoRect(stream, canvas),
             QRect(640, 0, 3840, 2160));

    const auto left = PlankPresentation::sliceForOutput(
                stream, canvas, QRect(0, 0, 2560, 2160));
    const auto right = PlankPresentation::sliceForOutput(
                stream, canvas, QRect(2560, 0, 2560, 2160));
    QCOMPARE(left.sourceRect, QRectF(0, 0, 1920, 2160));
    QCOMPARE(left.destinationRect, QRect(640, 0, 1920, 2160));
    QCOMPARE(right.sourceRect, QRectF(1920, 0, 1920, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 1920, 2160));
}

void TestPlankPresentation::mapsEachWindowIntoOneStreamCanvas()
{
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(1280, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(0, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(1280, 1080));

    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(0, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(2560, 1080));
}

void TestPlankPresentation::preservesMappingWithScaledLogicalWindows()
{
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(1024, 864), QSize(2048, 1728),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(3840, 1080));

    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                point, QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), QSize(2048, 1728),
                windowPoint));
    QCOMPARE(windowPoint, QPointF(1024, 864));
}

void TestPlankPresentation::mapsCursorIntoSingleOutput()
{
    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(960, 540), QSize(1920, 1080), QSize(1920, 1080),
                QRect(0, 0, 1920, 1080), QSize(1920, 1080), windowPoint));
    QCOMPARE(windowPoint, QPointF(960, 540));
}

void TestPlankPresentation::mapsCursorAcrossDualOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(2559, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(2559, 1080));
    QVERIFY(!PlankPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(2560, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
}

void TestPlankPresentation::mapsCursorAcrossAsymmetricOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(3839, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(3839, 1080));
    QVERIFY(!PlankPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(5119, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(1279, 1080));
}

void TestPlankPresentation::aspectFitMatchesRendererRounding()
{
    // Renderers have always letterboxed with ceil() on the fitted dimension
    // and halved the remaining band with integer division
    // (StreamUtils::scaleSourceToDestinationSurface, which now delegates to
    // aspectFitRect). 1080 * 1001 / 1920 = 563.06 must become 564, not 563.
    QCOMPARE(PlankPresentation::aspectFitRect(QSize(1920, 1080),
                                              QRect(0, 0, 1001, 1001)),
             QRect(0, 218, 1001, 564));
    QCOMPARE(PlankPresentation::videoRect(QSize(1920, 1080), QSize(1001, 1001)),
             QRect(0, 218, 1001, 564));
    // Offset destinations keep their origin.
    QCOMPARE(PlankPresentation::aspectFitRect(QSize(1920, 1080),
                                              QRect(10, 20, 2560, 1080)),
             QRect(330, 20, 1920, 1080));
    QVERIFY(PlankPresentation::aspectFitRect(QSize(0, 1080),
                                             QRect(0, 0, 100, 100)).isEmpty());
    QVERIFY(PlankPresentation::aspectFitRect(QSize(1920, 1080),
                                             QRect(0, 0, 0, 100)).isEmpty());
}

void TestPlankPresentation::fullscreenAfterStaleWindowedSnapshot()
{
    // macOS native fullscreen is asynchronous: the layout snapshot still holds
    // the 1536x864 windowed canvas while the window is already 1920x1200.
    const QSize stream(1920, 1080);
    const auto geometry = singleOutput(QSize(1536, 864), QSize(1920, 1200),
                                       QSize(1920, 1200));
    QVERIFY(geometry.live);
    QCOMPARE(geometry.canvasSize, QSize(1920, 1200));
    QCOMPARE(PlankPresentation::videoRect(stream, geometry.canvasSize),
             QRect(0, 60, 1920, 1080));

    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(960, 60), geometry, stream, point, false));
    QCOMPARE(point, QPointF(960, 0));
    // The last visible row, and a clamped point on the lower bar edge.
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(960, 1139), geometry, stream, point, false));
    QCOMPARE(point, QPointF(960, 1079));
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(960, 1140), geometry, stream, point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(960, 1140), geometry, stream, point, true));
    QCOMPARE(point, QPointF(960, 1080));
    // Inside the black bar: not video, so presses there are not forwarded.
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(960, 30), geometry, stream, point, false));

    // The stale snapshot (the regression) sent (960, 54) for the top edge
    // and treated the bar as video.
    const auto stale = singleOutput(QSize(1536, 864), QSize(1920, 1200),
                                    QSize(1920, 1200), false);
    QVERIFY(!stale.live);
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(960, 60), stale, stream, point, false));
    QCOMPARE(point, QPointF(960, 54));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(960, 30), stale, stream, point, false));
}

void TestPlankPresentation::windowedAfterStaleFullscreenSnapshot()
{
    // Leaving fullscreen leaves a 1920x1200 snapshot on a 1536x864 window.
    const QSize stream(1920, 1080);
    const auto geometry = singleOutput(QSize(1920, 1200), QSize(1536, 864),
                                       QSize(1536, 864));
    QPointF point;
    // (768, 43) is inside the picture; the stale snapshot classed it as
    // letterbox (y < 43.2) and dropped clicks there.
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(768, 43), geometry, stream, point, false));
    QCOMPARE(point, QPointF(960, 53.75));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(768, 0), geometry, stream, point, false));
    QCOMPARE(point, QPointF(960, 0));

    const auto stale = singleOutput(QSize(1920, 1200), QSize(1536, 864),
                                    QSize(1536, 864), false);
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(768, 43), stale, stream, point, false));
}

void TestPlankPresentation::windowedStartMapsContentArea()
{
    // A session that starts windowed: SDL reports the content size
    // (1536x864), not the 1536x896 frame with its title bar, and the renderer
    // draws into that same content area. Snapshot and live agree.
    const QSize stream(1920, 1080);
    const auto live = singleOutput(QSize(1536, 864), QSize(1536, 864),
                                   QSize(1536, 864));
    const auto snapshot = singleOutput(QSize(1536, 864), QSize(1536, 864),
                                       QSize(1536, 864), false);
    QCOMPARE(PlankPresentation::videoRect(stream, live.canvasSize),
             QRect(0, 0, 1536, 864));
    for (const QPointF& windowPoint : {QPointF(0, 0), QPointF(768, 432),
                                       QPointF(1535, 863)}) {
        QPointF livePoint;
        QPointF snapshotPoint;
        QVERIFY(PlankPresentation::mapWindowPointToStream(
                    windowPoint, live, stream, livePoint, false));
        QVERIFY(PlankPresentation::mapWindowPointToStream(
                    windowPoint, snapshot, stream, snapshotPoint, false));
        QCOMPARE(livePoint, snapshotPoint);
        QCOMPARE(livePoint, QPointF(windowPoint.x() * 1.25,
                                    windowPoint.y() * 1.25));
    }
}

void TestPlankPresentation::retinaWindowMapsThroughDrawable()
{
    // 1512x945 pt window at 2x after a resize from 1210x680 pt.
    const QSize stream(1920, 1080);
    const auto geometry = singleOutput(QSize(2420, 1360), QSize(1512, 945),
                                       QSize(3024, 1890));
    QCOMPARE(PlankPresentation::videoRect(stream, geometry.canvasSize),
             QRect(0, 94, 3024, 1701));

    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(756, 47.25), geometry, stream, point, false));
    QCOMPARE(point.x(), 960.0);
    QVERIFY(point.y() >= 0.0 && point.y() < 0.5);
    QCOMPARE(PlankPresentation::absoluteStreamPosition(point, stream),
             QPoint(960, 0));
    // Fractional points carry drawable-pixel precision: half a point is one
    // drawable pixel, about 0.63 host pixels here.
    QPointF next;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(756, 47.75), geometry, stream, next, false));
    QVERIFY(qAbs((next.y() - point.y()) - 1080.0 / 1701.0) < 1e-9);
}

void TestPlankPresentation::pillarboxAndFourByThree()
{
    const QSize stream(1920, 1080);
    QPointF point;

    // 21:9: 320 px pillars each side.
    const auto wide = singleOutput(QSize(1920, 1080), QSize(2560, 1080),
                                   QSize(2560, 1080));
    QCOMPARE(PlankPresentation::videoRect(stream, wide.canvasSize),
             QRect(320, 0, 1920, 1080));
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(319, 540), wide, stream, point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(320, 540), wide, stream, point, false));
    QCOMPARE(point, QPointF(0, 540));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(2239, 540), wide, stream, point, false));
    QCOMPARE(point, QPointF(1919, 540));
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(2240, 540), wide, stream, point, false));

    // 4:3: 150 px bars top and bottom.
    const auto square = singleOutput(QSize(1920, 1080), QSize(1600, 1200),
                                     QSize(1600, 1200));
    QCOMPARE(PlankPresentation::videoRect(stream, square.canvasSize),
             QRect(0, 150, 1600, 900));
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(800, 149), square, stream, point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(800, 150), square, stream, point, false));
    QCOMPARE(point, QPointF(960, 0));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(800, 600), square, stream, point, false));
    QCOMPARE(point, QPointF(960, 540));
}

void TestPlankPresentation::bestFitStreamOnRetinaPanel()
{
    // A 16:10 best-fit stream on a 3024x1964 panel (1512x982 pt at 2x).
    const QSize stream(2560, 1600);
    const auto geometry = singleOutput(QSize(3024, 1964), QSize(1512, 982),
                                       QSize(3024, 1964));
    QCOMPARE(PlankPresentation::videoRect(stream, geometry.canvasSize),
             QRect(0, 37, 3024, 1890));
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(756, 491), geometry, stream, point, false));
    QCOMPARE(point, QPointF(1280, 800));
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
                QPointF(756, 18), geometry, stream, point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(756, 18.5), geometry, stream, point, false));
    QCOMPARE(point, QPointF(1280, 0));
}

void TestPlankPresentation::roundTripsAtScaleOneAndTwo()
{
    const QSize stream(1920, 1080);
    const struct {
        QSize window;
        QSize drawable;
    } cases[] = {
        {QSize(1920, 1200), QSize(1920, 1200)},
        {QSize(1512, 945), QSize(3024, 1890)},
        {QSize(1280, 1024), QSize(2560, 2048)},
    };
    for (const auto& testCase : cases) {
        const auto geometry = singleOutput(QSize(800, 600), testCase.window,
                                           testCase.drawable);
        for (const QPointF& streamPoint : {QPointF(0, 0), QPointF(960, 540),
                                           QPointF(1919, 1079),
                                           QPointF(123.5, 987.25)}) {
            QPointF windowPoint;
            QVERIFY(PlankPresentation::mapStreamPointToWindow(
                        streamPoint, stream, geometry, windowPoint));
            QPointF back;
            QVERIFY(PlankPresentation::mapWindowPointToStream(
                        windowPoint, geometry, stream, back, false));
            QVERIFY2(qAbs(back.x() - streamPoint.x()) < 1e-6 &&
                     qAbs(back.y() - streamPoint.y()) < 1e-6,
                     qPrintable(QStringLiteral("%1,%2 -> %3,%4")
                                .arg(streamPoint.x()).arg(streamPoint.y())
                                .arg(back.x()).arg(back.y())));
        }
    }
}

void TestPlankPresentation::clampsAbsolutePositionInsideImage()
{
    const QSize stream(1920, 1080);
    QCOMPARE(PlankPresentation::absoluteStreamPosition(QPointF(1920, 1080),
                                                       stream),
             QPoint(1919, 1079));
    QCOMPARE(PlankPresentation::absoluteStreamPosition(QPointF(1919.6, 1079.6),
                                                       stream),
             QPoint(1919, 1079));
    QCOMPARE(PlankPresentation::absoluteStreamPosition(QPointF(-3, -0.4),
                                                       stream),
             QPoint(0, 0));
    QCOMPARE(PlankPresentation::absoluteStreamPosition(QPointF(959.5, 0.49),
                                                       stream),
             QPoint(960, 0));

    // A click on the right/bottom edge with clamping still lands inside.
    const auto geometry = singleOutput(QSize(1920, 1200), QSize(1920, 1200),
                                       QSize(1920, 1200));
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(1920, 1199), geometry, stream, point, true));
    QCOMPARE(PlankPresentation::absoluteStreamPosition(point, stream),
             QPoint(1919, 1079));
}

void TestPlankPresentation::snapshotRenderersAndMultiOutputKeepLayout()
{
    // Renderers that letterbox against the layout they copied at init
    // (Linux Vulkan/EGL) keep the snapshot, so input keeps matching them.
    const auto snapshot = singleOutput(QSize(1536, 864), QSize(1920, 1200),
                                       QSize(1920, 1200), false);
    QVERIFY(!snapshot.live);
    QCOMPARE(snapshot.canvasSize, QSize(1536, 864));
    QCOMPARE(snapshot.canvasRect, QRect(0, 0, 1536, 864));

    // Multi-output spans always keep the shared canvas model.
    PlankPresentationLayout layout;
    layout.canvasSize = QSize(5120, 2160);
    layout.outputs.append({nullptr, QRect(0, 0, 2560, 2160), true});
    layout.outputs.append({nullptr, QRect(2560, 0, 2560, 2160), false});
    const auto right = PlankPresentation::outputGeometry(
                layout, layout.outputs.at(1), QSize(2560, 2160),
                QSize(2560, 2160), true);
    QVERIFY(!right.live);
    QCOMPARE(right.canvasSize, QSize(5120, 2160));
    QCOMPARE(right.canvasRect, QRect(2560, 0, 2560, 2160));

    // No drawable yet (window being created): fall back to the snapshot.
    const auto noDrawable = singleOutput(QSize(1536, 864), QSize(1536, 864),
                                         QSize(0, 0));
    QVERIFY(!noDrawable.live);
    QCOMPARE(noDrawable.canvasSize, QSize(1536, 864));

    QVERIFY(!singleOutput(QSize(1536, 864), QSize(0, 0),
                          QSize(1536, 864)).isValid());
}

void TestPlankPresentation::videoRectInWindowPoints()
{
    const QSize stream(1920, 1080);
    const auto retina = singleOutput(QSize(2420, 1360), QSize(1512, 945),
                                     QSize(3024, 1890));
    QCOMPARE(PlankPresentation::videoRectInWindow(stream, retina),
             QRectF(0, 47, 1512, 850.5));

    const auto wide = singleOutput(QSize(1920, 1080), QSize(2560, 1080),
                                   QSize(2560, 1080));
    QCOMPARE(PlankPresentation::videoRectInWindow(stream, wide),
             QRectF(320, 0, 1920, 1080));
}

void TestPlankPresentation::decodedSizeReportedOnlyForFrameFittingRenderers()
{
    // A renderer that fits the negotiated size (d3d11va, dxva2, vdpau, mmal)
    // stretches a 2560x1600 frame into its 16:9 rect: input must stay on the
    // negotiated size, so nothing is reported.
    PlankDecodedFrameSizeTracker negotiatedFit;
    QVERIFY(!negotiatedFit.shouldReport(2560, 1600, false));
    QVERIFY(!negotiatedFit.shouldReport(2560, 1600, false));

    // A renderer that fits frame->width/height reports once per change.
    PlankDecodedFrameSizeTracker frameFit;
    QVERIFY(frameFit.shouldReport(2560, 1600, true));
    QVERIFY(!frameFit.shouldReport(2560, 1600, true));
    QVERIFY(frameFit.shouldReport(1920, 1080, true));
    QVERIFY(!frameFit.shouldReport(0, 1080, true));
    QVERIFY(!frameFit.shouldReport(1920, -1, true));
}

void TestPlankPresentation::decodedSizeReportedAgainAfterReset()
{
    // Reconnect with a retained decoder: the session re-applies the
    // negotiated size to input, the decoder resumes with frames of the same
    // decoded size, and must report it again.
    PlankDecodedFrameSizeTracker tracker;
    QVERIFY(tracker.shouldReport(2560, 1600, true));
    QVERIFY(!tracker.shouldReport(2560, 1600, true));
    tracker.reset();
    QVERIFY(tracker.shouldReport(2560, 1600, true));
    QVERIFY(!tracker.shouldReport(2560, 1600, true));
}


namespace {

PlankPresentationOutput sourceOutput(const QRectF& source, const QRect& desktop, bool primary = false)
{
    PlankPresentationOutput output;
    output.primary = primary;
    output.sourceRect = source;
    output.desktopRect = desktop;
    return output;
}

}

void TestPlankPresentation::sourceRectsSliceEachWindow()
{
    // A 14" MacBook viewport beside a UHD monitor, and a stacked third and
    // fourth display: 7280x3600 desktop streamed 1:1.
    const QSize desktop(7280, 3600);
    const QVector<QRect> rects {QRect(0, 0, 3840, 2160), QRect(3840, 0, 3440, 1440),
                                QRect(0, 2160, 2560, 1440), QRect(2560, 2160, 1920, 1200)};
    for (const QRect& rect : rects) {
        const QRectF source = PlankPresentation::sourceRectInStream(rect, desktop, desktop);
        QCOMPARE(source, QRectF(rect));
        // A window of exactly that many pixels shows it 1:1.
        const auto slice = PlankPresentation::sliceForSource(desktop, source, rect.size());
        QVERIFY(slice.visible);
        QCOMPARE(slice.sourceRect, QRectF(rect));
        QCOMPARE(slice.destinationRect, QRect(QPoint(0, 0), rect.size()));
    }
    // A stream scaled to half the desktop scales every source rectangle.
    QCOMPARE(PlankPresentation::sourceRectInStream(rects.at(1), desktop, QSize(3640, 1800)),
             QRectF(1920, 0, 1720, 720));
    // A rectangle that runs past the stream is clipped to it.
    const auto clipped = PlankPresentation::sliceForSource(QSize(3840, 2160), QRectF(3000, 0, 1000, 2160),
                                                           QSize(840, 2160));
    QCOMPARE(clipped.sourceRect, QRectF(3000, 0, 840, 2160));
}

void TestPlankPresentation::sourceRectsFitMismatchedWindows()
{
    // A 3024x1890 viewport shown on a 3024x1964 drawable: letterboxed in
    // that window only, centred.
    const auto slice = PlankPresentation::sliceForSource(QSize(6864, 2160), QRectF(0, 270, 3024, 1890),
                                                         QSize(3024, 1964));
    QCOMPARE(slice.destinationRect, QRect(0, 37, 3024, 1890));
    // "Looks like" on a Retina window: a 1512x944 desktop fills 3024x1888 exactly.
    const auto retina = PlankPresentation::sliceForSource(QSize(1512, 944), QRectF(0, 0, 1512, 944),
                                                          QSize(3024, 1888));
    QCOMPARE(retina.destinationRect, QRect(0, 0, 3024, 1888));
}

void TestPlankPresentation::packedCaptureMapsPointerToDesktop()
{
    // Three UHD monitors side by side (11520x2160 desktop) packed by the host
    // into a 7680x4320 capture: the third monitor's pixels sit below the
    // first. Pointer positions must still land on the desktop.
    const QSize stream(7680, 4320);
    const PlankPresentationOutput third = sourceOutput(QRectF(0, 2160, 3840, 2160), QRect(7680, 0, 3840, 2160));
    QPointF desktopPoint;
    // Window in points (1920x1080) on a 2x drawable.
    QVERIFY(PlankPresentation::mapWindowPointToDesktop(QPointF(960, 540), QSize(1920, 1080), QSize(3840, 2160),
                                                       third, stream, desktopPoint, false));
    QCOMPARE(desktopPoint, QPointF(7680 + 1920, 1080));
    QVERIFY(PlankPresentation::mapWindowPointToDesktop(QPointF(0, 0), QSize(1920, 1080), QSize(3840, 2160),
                                                       third, stream, desktopPoint, false));
    QCOMPARE(desktopPoint, QPointF(7680, 0));
    QCOMPARE(PlankPresentation::absoluteDesktopPosition(QPointF(11520, 2160), QSize(11520, 2160)),
             QPoint(11519, 2159));
    // In a letterbox band: only with clamping (a drag that leaves the video).
    const PlankPresentationOutput notch = sourceOutput(QRectF(0, 270, 3024, 1890), QRect(0, 270, 3024, 1890));
    QVERIFY(!PlankPresentation::mapWindowPointToDesktop(QPointF(100, 5), QSize(1512, 982), QSize(3024, 1964),
                                                        notch, QSize(6864, 2160), desktopPoint, false));
    QVERIFY(PlankPresentation::mapWindowPointToDesktop(QPointF(100, 5), QSize(1512, 982), QSize(3024, 1964),
                                                       notch, QSize(6864, 2160), desktopPoint, true));
    QCOMPARE(desktopPoint, QPointF(200, 270));
}

void TestPlankPresentation::remoteCursorFindsItsWindow()
{
    const QSize stream(6864, 2160);
    const PlankPresentationOutput laptop = sourceOutput(QRectF(0, 270, 3024, 1890), QRect(0, 270, 3024, 1890), true);
    const PlankPresentationOutput uhd = sourceOutput(QRectF(3024, 0, 3840, 2160), QRect(3024, 0, 3840, 2160));
    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToSourceWindow(QPointF(4944, 1080), stream, uhd, QSize(1920, 1080),
                                                            QSize(3840, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(960, 540));
    QVERIFY(!PlankPresentation::mapStreamPointToSourceWindow(QPointF(4944, 1080), stream, laptop, QSize(1512, 945),
                                                             QSize(3024, 1890), windowPoint));
    // The black corner above the laptop belongs to no window.
    QVERIFY(!PlankPresentation::mapStreamPointToSourceWindow(QPointF(100, 100), stream, laptop, QSize(1512, 945),
                                                             QSize(3024, 1890), windowPoint));
    QVERIFY(PlankPresentation::mapStreamPointToSourceWindow(QPointF(1512, 1215), stream, laptop, QSize(1512, 945),
                                                            QSize(3024, 1890), windowPoint));
    QCOMPARE(windowPoint, QPointF(756, 472.5));
    // Round trip through the desktop.
    QPointF desktopPoint;
    QVERIFY(PlankPresentation::mapWindowPointToDesktop(windowPoint, QSize(1512, 945), QSize(3024, 1890), laptop,
                                                       stream, desktopPoint, false));
    QCOMPARE(desktopPoint, QPointF(1512, 1215));
}

void TestPlankPresentation::sourceRectsMatchLegacyExactLayout()
{
    // Today's Wayland two-output layout (5120x2160 canvas, two 2560x2160
    // outputs, stream of the canvas size) expressed with source rectangles:
    // every slice and every pointer position is the same.
    const QSize stream(5120, 2160);
    const QVector<QRect> canvasRects {QRect(0, 0, 2560, 2160), QRect(2560, 0, 2560, 2160)};
    for (const QRect& canvasRect : canvasRects) {
        const auto legacy = PlankPresentation::sliceForOutput(stream, stream, canvasRect);
        const auto source = PlankPresentation::sliceForSource(stream, legacy.sourceRect, canvasRect.size());
        QCOMPARE(source.sourceRect, legacy.sourceRect);
        QCOMPARE(source.destinationRect, legacy.destinationRect);
        const PlankPresentationOutput output = sourceOutput(legacy.sourceRect, canvasRect);
        for (const QSize windowSize : {canvasRect.size(), QSize(1280, 1080)}) {
            for (int y = 0; y <= windowSize.height(); y += windowSize.height() / 8) {
                for (int x = 0; x <= windowSize.width(); x += windowSize.width() / 8) {
                    QPointF legacyPoint;
                    QPointF desktopPoint;
                    const bool legacyOk = PlankPresentation::mapWindowPointToStream(
                                QPointF(x, y), windowSize, stream, stream, canvasRect, legacyPoint, true);
                    const bool sourceOk = PlankPresentation::mapWindowPointToDesktop(
                                QPointF(x, y), windowSize, canvasRect.size(), output, stream, desktopPoint, true);
                    QCOMPARE(sourceOk, legacyOk);
                    QVERIFY2(qAbs(desktopPoint.x() - legacyPoint.x()) < 1e-6 &&
                             qAbs(desktopPoint.y() - legacyPoint.y()) < 1e-6,
                             qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
                }
            }
        }
    }
}

void TestPlankPresentation::layoutRequiresCompleteSourceRects()
{
    PlankPresentationLayout layout;
    layout.canvasSize = QSize(6864, 2160);
    layout.outputs = {sourceOutput(QRectF(0, 270, 3024, 1890), QRect(0, 270, 3024, 1890), true),
                      sourceOutput(QRectF(3024, 0, 3840, 2160), QRect(3024, 0, 3840, 2160))};
    QVERIFY(!layout.usesSourceRects()); // no desktop size yet
    layout.desktopSize = QSize(6864, 2160);
    QVERIFY(layout.usesSourceRects());
    layout.outputs[1].sourceRect = QRectF();
    QVERIFY(!layout.usesSourceRects());
    // The legacy aggregate still builds a canvas output without them.
    PlankPresentationOutput legacy {nullptr, QRect(0, 0, 1920, 1080), true};
    QVERIFY(!legacy.sourceRect.isValid());
    QVERIFY(!legacy.desktopRect.isValid());
}

void TestPlankPresentation::packedThreeUhdThirdScreenMapsToTheDesktop()
{
    // The packing vector "three-uhd-row": an 11520x2160 desktop captured as
    // 7680x4320, the third screen's pixels on the second row. The host
    // publishes that slot as capture_rect (source_rect stays the desktop
    // rectangle); the Session presents the window from it.
    const QSize capture(7680, 4320);
    const QSize desktop(11520, 2160);
    PlankPresentationLayout layout;
    layout.desktopSize = desktop;
    layout.canvasSize = desktop;
    const QRect captureRects[3] = {QRect(0, 0, 3840, 2160), QRect(3840, 0, 3840, 2160), QRect(0, 2160, 3840, 2160)};
    for (int index = 0; index < 3; ++index) {
        PlankPresentationOutput output(nullptr, QRect(3840 * index, 0, 3840, 2160), index == 0);
        output.sourceRect = PlankPresentation::sourceRectInStream(captureRects[index], capture, capture);
        output.desktopRect = QRect(3840 * index, 0, 3840, 2160);
        layout.outputs.append(output);
    }
    QVERIFY(layout.usesSourceRects());
    const PlankPresentationOutput& third = layout.outputs.at(2);
    // Its window (1920x1080 points on a 2x panel) shows capture rows from 2160 down...
    const auto slice = PlankPresentation::sliceForSource(capture, third.sourceRect, QSize(3840, 2160));
    QVERIFY(slice.visible);
    QVERIFY(slice.sourceRect.top() >= 2160);
    QCOMPARE(slice.sourceRect, QRectF(0, 2160, 3840, 2160));
    // ...and every point in it lands on the desktop at x >= 7680.
    for (const QPointF point : {QPointF(0, 0), QPointF(960, 540), QPointF(1919, 1079)}) {
        QPointF desktopPoint;
        QVERIFY(PlankPresentation::mapWindowPointToDesktop(point, QSize(1920, 1080), QSize(3840, 2160), third,
                                                           capture, desktopPoint, false));
        QVERIFY(desktopPoint.x() >= 7680);
        const QPoint absolute = PlankPresentation::absoluteDesktopPosition(desktopPoint, layout.desktopSize);
        QVERIFY(absolute.x() >= 7680 && absolute.x() < 11520);
        QVERIFY(absolute.y() >= 0 && absolute.y() < 2160);
    }
    // The remote cursor at desktop-row pixel (1920, 3240) of the capture is on that window.
    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToSourceWindow(QPointF(1920, 3240), capture, third, QSize(1920, 1080),
                                                            QSize(3840, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(960, 540));
    QVERIFY(!PlankPresentation::mapStreamPointToSourceWindow(QPointF(1920, 3240), capture, layout.outputs.at(0),
                                                             QSize(1920, 1080), QSize(3840, 2160), windowPoint));
}

void TestPlankPresentation::capturedDragCrossesBetweenDisplays()
{
    const QVector<QRect> windows {QRect(0, 0, 1920, 1080), QRect(1920, 0, 1920, 1080)};
    // A Retina client: events remain in logical coordinates on the first
    // window while the held-button drag crosses into the second monitor.
    PlankPresentationOutput right;
    right.canvasRect = QRect(3840, 0, 3840, 2160);
    right.desktopRect = right.canvasRect;
    right.sourceRect = QRectF(right.canvasRect);
    QPointF local, desktop;
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(2100.5, 300.25), QPoint(0, 0), windows, local), 1);
    QCOMPARE(local, QPointF(180.5, 300.25));
    QVERIFY(PlankPresentation::mapWindowPointToDesktop(local, QSize(1920, 1080), QSize(3840, 2160),
                                                       right, QSize(7680, 2160), desktop, false));
    QCOMPARE(desktop, QPointF(4201, 600.5));
    // The same native capture can cross back without changing mouse focus.
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(-0.5, 300), QPoint(1920, 0), windows, local), 0);
    QCOMPARE(local, QPointF(1919.5, 300));
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(1920, 300), QPoint(0, 0), windows, local), 1);
    QCOMPARE(local, QPointF(0, 300));
}

void TestPlankPresentation::capturedDragHonoursLogicalBoundsAndGaps()
{
    const QVector<QRect> windows {QRect(-2560, -200, 2560, 1440), QRect(0, 0, 1920, 1080),
                                  QRect(0, 1080, 1920, 1080), QRect()};
    QPointF local;
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(-50.25, 10), QPoint(0, 0), windows, local), 0);
    QCOMPARE(local, QPointF(2509.75, 210));
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(150, 1150), QPoint(0, 0), windows, local), 2);
    QCOMPARE(local, QPointF(150, 70));
    // A local-only gap or hidden output is not another remote display.
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(150, -20), QPoint(0, 0), windows, local), -1);
    QCOMPARE(PlankPresentation::capturedPointerTarget(QPointF(2000, 100), QPoint(0, 0), windows, local), -1);
}

QTEST_APPLESS_MAIN(TestPlankPresentation)

#include "test_plankpresentation.moc"
