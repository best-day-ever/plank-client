#include <QtTest>

#include "streaming/stationconnectpresentation.h"

class TestStationConnectPresentation : public QObject
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
};

void TestStationConnectPresentation::exactDualOutputSlices()
{
    const QSize size(5120, 2160);
    const auto left = StationConnectPresentation::sliceForOutput(
                size, size, QRect(0, 0, 2560, 2160));
    const auto right = StationConnectPresentation::sliceForOutput(
                size, size, QRect(2560, 0, 2560, 2160));

    QVERIFY(left.visible);
    QCOMPARE(left.sourceRect, QRectF(0, 0, 2560, 2160));
    QCOMPARE(left.destinationRect, QRect(0, 0, 2560, 2160));
    QVERIFY(right.visible);
    QCOMPARE(right.sourceRect, QRectF(2560, 0, 2560, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 2560, 2160));
}

void TestStationConnectPresentation::letterboxedDualOutputSlices()
{
    const QSize stream(3840, 2160);
    const QSize canvas(5120, 2160);
    QCOMPARE(StationConnectPresentation::videoRect(stream, canvas),
             QRect(640, 0, 3840, 2160));

    const auto left = StationConnectPresentation::sliceForOutput(
                stream, canvas, QRect(0, 0, 2560, 2160));
    const auto right = StationConnectPresentation::sliceForOutput(
                stream, canvas, QRect(2560, 0, 2560, 2160));
    QCOMPARE(left.sourceRect, QRectF(0, 0, 1920, 2160));
    QCOMPARE(left.destinationRect, QRect(640, 0, 1920, 2160));
    QCOMPARE(right.sourceRect, QRectF(1920, 0, 1920, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 1920, 2160));
}

void TestStationConnectPresentation::mapsEachWindowIntoOneStreamCanvas()
{
    QPointF point;
    QVERIFY(StationConnectPresentation::mapWindowPointToStream(
                QPointF(1280, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(0, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(1280, 1080));

    QVERIFY(StationConnectPresentation::mapWindowPointToStream(
                QPointF(0, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(2560, 1080));
}

void TestStationConnectPresentation::preservesMappingWithScaledLogicalWindows()
{
    QPointF point;
    QVERIFY(StationConnectPresentation::mapWindowPointToStream(
                QPointF(1024, 864), QSize(2048, 1728),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(3840, 1080));

    QPointF windowPoint;
    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                point, QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), QSize(2048, 1728),
                windowPoint));
    QCOMPARE(windowPoint, QPointF(1024, 864));
}

void TestStationConnectPresentation::mapsCursorIntoSingleOutput()
{
    QPointF windowPoint;
    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(960, 540), QSize(1920, 1080), QSize(1920, 1080),
                QRect(0, 0, 1920, 1080), QSize(1920, 1080), windowPoint));
    QCOMPARE(windowPoint, QPointF(960, 540));
}

void TestStationConnectPresentation::mapsCursorAcrossDualOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(2559, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(2559, 1080));
    QVERIFY(!StationConnectPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));

    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(2560, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
}

void TestStationConnectPresentation::mapsCursorAcrossAsymmetricOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(3839, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(3839, 1080));
    QVERIFY(!StationConnectPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));

    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
    QVERIFY(StationConnectPresentation::mapStreamPointToWindow(
                QPointF(5119, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(1279, 1080));
}

QTEST_APPLESS_MAIN(TestStationConnectPresentation)

#include "test_stationconnectpresentation.moc"
