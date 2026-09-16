#include <QtTest>

#include "streaming/plankpresentation.h"

class TestPlankPresentation : public QObject
{
    Q_OBJECT

private slots:
    void exactDualOutputSlices();
    void letterboxedDualOutputSlices();
    void mapsEachWindowIntoOneStreamCanvas();
    void preservesMappingWithScaledLogicalWindows();
    void mapsCursorIntoSingleOutput();
    void mapsCursorAcrossDualOutputSeam();
    void mapsCursorAcrossAsymmetricOutputSeam();
    void mapsMixedRetinaDrawable();
    void scalesLetterboxToBackingPixels();
    void rejectsInvisibleOrInvalidDrawables();
};

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

void TestPlankPresentation::mapsMixedRetinaDrawable()
{
    const QSize canvas(6016, 2234);
    const QRect laptop(2560, 0, 3456, 2234);
    const auto slice = PlankPresentation::sliceForDrawable(canvas, canvas, laptop,
                                                          QSize(3456, 2234));
    QCOMPARE(slice.sourceRect, QRectF(laptop));
    QCOMPARE(slice.destinationRect, QRect(0, 0, 3456, 2234));
    QPointF streamPoint, windowPoint;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
        QPointF(864, 558.5), QSize(1728, 1117), canvas, canvas, laptop,
        streamPoint, false));
    QCOMPARE(streamPoint, QPointF(4288, 1117));
    QVERIFY(PlankPresentation::mapStreamPointToWindow(streamPoint, canvas, canvas,
        laptop, QSize(1728, 1117), windowPoint));
    QCOMPARE(windowPoint, QPointF(864, 558.5));
}

void TestPlankPresentation::scalesLetterboxToBackingPixels()
{
    const auto slice = PlankPresentation::sliceForDrawable(
        QSize(3840, 2160), QSize(5120, 2160), QRect(0, 0, 2560, 2160),
        QSize(1280, 1080));
    QVERIFY(slice.visible);
    QCOMPARE(slice.sourceRect, QRectF(0, 0, 1920, 2160));
    QCOMPARE(slice.destinationRect, QRect(320, 0, 960, 1080));
    QPointF point;
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
        QPointF(100, 500), QSize(1280, 1080), QSize(3840, 2160),
        QSize(5120, 2160), QRect(0, 0, 2560, 2160), point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
        QPointF(800, 540), QSize(1280, 1080), QSize(3840, 2160),
        QSize(5120, 2160), QRect(0, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(960, 1080));
}

void TestPlankPresentation::rejectsInvisibleOrInvalidDrawables()
{
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 2160),
        QSize(5120, 2160), QRect(0, 0, 1280, 2160), QSize(1280, 2160)).visible);
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 1080),
        QSize(1920, 1080), QRect(), QSize(1920, 1080)).visible);
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 1080),
        QSize(1920, 1080), QRect(0, 0, 1920, 1080), QSize(0, 0)).visible);
}

QTEST_APPLESS_MAIN(TestPlankPresentation)

#include "test_plankpresentation.moc"
