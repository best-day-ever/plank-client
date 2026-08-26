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
    void isolatesVisibleNativeChildFromParentMotion();
    void keepsNativeButtonSequenceOutOfSdlPath();
    void keepsRemotePressReleaseTogether();
    void keepsLocalPressReleaseTogether();
    void keepsMultiButtonSequenceWithFirstOwner();
    void confinesPointerOnlyWithoutToolbar();
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

void TestStationConnectToolbarLogic::isolatesVisibleNativeChildFromParentMotion()
{
    QVERIFY(!StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, false));
    QVERIFY(!StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
}

void TestStationConnectToolbarLogic::keepsNativeButtonSequenceOutOfSdlPath()
{
    QVERIFY(!StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, true));
    QVERIFY(!StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
    QVERIFY(StationConnectToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, true));
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

void TestStationConnectToolbarLogic::confinesPointerOnlyWithoutToolbar()
{
    QVERIFY(StationConnectPointerLogic::shouldApplyPointerConfinement(false));
    QVERIFY(!StationConnectPointerLogic::shouldApplyPointerConfinement(true));
}

QTEST_APPLESS_MAIN(TestStationConnectToolbarLogic)

#include "test_stationconnecttoolbarlogic.moc"
