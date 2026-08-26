#include <QtTest>

#include "stationconnecttoolbarlogic.h"
#include "input/stationconnectpointerlogic.h"

using StationConnectToolbarLogic::ButtonRouter;

class TestStationConnectToolbarLogic : public QObject
{
    Q_OBJECT

private slots:
    void resolvesReportedAndDerivedDensity();
    void alignsLogicalHitRectWithPhysicalSurface_data();
    void alignsLogicalHitRectWithPhysicalSurface();
    void preservesHorizontalPositionAcrossScaleChanges();
    void keepsNativeChildCoordinatesIndependentOfDisplayScale_data();
    void keepsNativeChildCoordinatesIndependentOfDisplayScale();
    void derivesNativeDragFromFixedPressOrigin();
    void keepsRemotePressReleaseTogether();
    void keepsLocalPressReleaseTogether();
    void keepsMultiButtonSequenceWithFirstOwner();
    void confinesToVideoWithoutToolbar();
    void reachesWindowEdgeWithToolbar();
};

void TestStationConnectToolbarLogic::resolvesReportedAndDerivedDensity()
{
    QCOMPARE(StationConnectToolbarLogic::resolvePixelDensity(
                 1.25f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(StationConnectToolbarLogic::resolvePixelDensity(
                 0.0f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(StationConnectToolbarLogic::resolvePixelDensity(
                 0.0f, 0, 0, 0, 0), 1.0f);
}

void TestStationConnectToolbarLogic::alignsLogicalHitRectWithPhysicalSurface_data()
{
    QTest::addColumn<float>("density");
    QTest::newRow("100-percent") << 1.0f;
    QTest::newRow("125-percent") << 1.25f;
    QTest::newRow("150-percent") << 1.5f;
    QTest::newRow("200-percent") << 2.0f;
}

void TestStationConnectToolbarLogic::alignsLogicalHitRectWithPhysicalSurface()
{
    QFETCH(float, density);

    constexpr int logicalWindowWidth = 4096;
    constexpr int logicalToolbarWidth = 539;
    constexpr int logicalLeft = 2671;
    const int pixelWindowWidth = StationConnectToolbarLogic::physicalExtent(
                logicalWindowWidth, density);
    const int pixelToolbarWidth = StationConnectToolbarLogic::physicalExtent(
                logicalToolbarWidth, density);
    const float position = StationConnectToolbarLogic::horizontalPosition(
                logicalLeft, logicalWindowWidth, logicalToolbarWidth);
    const int renderedPixelLeft = qRound(
                position * (pixelWindowWidth - pixelToolbarWidth));
    const int expectedPixelLeft = qRound(logicalLeft * density);

    QVERIFY(qAbs(renderedPixelLeft - expectedPixelLeft) <= 1);
    QVERIFY(qAbs((renderedPixelLeft + pixelToolbarWidth) -
                 qRound((logicalLeft + logicalToolbarWidth) * density)) <= 1);
}

void TestStationConnectToolbarLogic::preservesHorizontalPositionAcrossScaleChanges()
{
    constexpr int toolbarWidth = 539;
    constexpr int oldWindowWidth = 3840;
    constexpr int oldLeft = 2476;
    constexpr int newWindowWidth = 3072;
    const float position = StationConnectToolbarLogic::horizontalPosition(
                oldLeft, oldWindowWidth, toolbarWidth);
    const int newLeft = StationConnectToolbarLogic::logicalLeftFromPosition(
                position, newWindowWidth, toolbarWidth);

    QVERIFY(qAbs(StationConnectToolbarLogic::horizontalPosition(
                     newLeft, newWindowWidth, toolbarWidth) - position) < 0.001f);
}

void TestStationConnectToolbarLogic::keepsNativeChildCoordinatesIndependentOfDisplayScale_data()
{
    QTest::addColumn<float>("displayScale");
    QTest::newRow("100-percent") << 1.0f;
    QTest::newRow("125-percent") << 1.25f;
    QTest::newRow("200-percent") << 2.0f;
}

void TestStationConnectToolbarLogic::keepsNativeChildCoordinatesIndependentOfDisplayScale()
{
    QFETCH(float, displayScale);
    Q_UNUSED(displayScale);

    constexpr int toolbarLeft = 2671;
    constexpr int localPointerX = 311;
    QCOMPARE(StationConnectToolbarLogic::nativePointerParentCoordinate(
                 toolbarLeft, localPointerX), 2982);
}

void TestStationConnectToolbarLogic::derivesNativeDragFromFixedPressOrigin()
{
    QCOMPARE(StationConnectToolbarLogic::nativeDragFinalLeft(
                 1200, 24, 224, 3840, 607), 1400);
    QCOMPARE(StationConnectToolbarLogic::nativeDragFinalLeft(
                 1200, 24, -2000, 3840, 607), 0);
    QCOMPARE(StationConnectToolbarLogic::nativeDragFinalLeft(
                 1200, 24, 4000, 3840, 607), 3233);
}

void TestStationConnectToolbarLogic::keepsRemotePressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, false), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(1, false, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Local);
}

void TestStationConnectToolbarLogic::keepsLocalPressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, true), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(1, false, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Remote);
}

void TestStationConnectToolbarLogic::keepsMultiButtonSequenceWithFirstOwner()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, true), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(3, true, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(1, false, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(3, false, false), ButtonRouter::Owner::Local);

    QCOMPARE(router.routeButton(1, true, false), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(3, true, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(1, false, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(3, false, true), ButtonRouter::Owner::Remote);
}

void TestStationConnectToolbarLogic::confinesToVideoWithoutToolbar()
{
    const StationConnectPointerLogic::Rect window = {0, 0, 3840, 2160};
    const StationConnectPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = StationConnectPointerLogic::pointerConfinementRect(
                window, video, false);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 228);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 1704);
}

void TestStationConnectToolbarLogic::reachesWindowEdgeWithToolbar()
{
    const StationConnectPointerLogic::Rect window = {0, 0, 3840, 2160};
    const StationConnectPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = StationConnectPointerLogic::pointerConfinementRect(
                window, video, true);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 0);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 2160);
}

QTEST_APPLESS_MAIN(TestStationConnectToolbarLogic)

#include "test_stationconnecttoolbarlogic.moc"
