#include <QtTest>

#include "planktoolbarlogic.h"
#include "input/plankpointerlogic.h"
#include "videopacketlosswindow.h"

using PlankToolbarLogic::ButtonRouter;

class TestPlankToolbarLogic : public QObject
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
    void confinesToVideoWithoutToolbar();
    void reachesWindowEdgeWithToolbar();
    void calculatesOnePreFecLossInterval();
    void resetsPreFecLossAfterCounterRestart();
    void retainsTenSecondPeakForBothStatsViews();
    void sharesPacketLossDisplayPrecision();
};

void TestPlankToolbarLogic::resolvesReportedAndDerivedDensity()
{
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 1.25f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 0.0f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 0.0f, 0, 0, 0, 0), 1.0f);
}

void TestPlankToolbarLogic::alignsLogicalHitRectWithPhysicalSurface_data()
{
    QTest::addColumn<float>("density");
    QTest::newRow("100-percent") << 1.0f;
    QTest::newRow("125-percent") << 1.25f;
    QTest::newRow("150-percent") << 1.5f;
    QTest::newRow("200-percent") << 2.0f;
}

void TestPlankToolbarLogic::alignsLogicalHitRectWithPhysicalSurface()
{
    QFETCH(float, density);

    constexpr int logicalWindowWidth = 4096;
    constexpr int logicalToolbarWidth = 539;
    constexpr int logicalLeft = 2671;
    const int pixelWindowWidth = PlankToolbarLogic::physicalExtent(
                logicalWindowWidth, density);
    const int pixelToolbarWidth = PlankToolbarLogic::physicalExtent(
                logicalToolbarWidth, density);
    const float position = PlankToolbarLogic::horizontalPosition(
                logicalLeft, logicalWindowWidth, logicalToolbarWidth);
    const int renderedPixelLeft = qRound(
                position * (pixelWindowWidth - pixelToolbarWidth));
    const int expectedPixelLeft = qRound(logicalLeft * density);

    QVERIFY(qAbs(renderedPixelLeft - expectedPixelLeft) <= 1);
    QVERIFY(qAbs((renderedPixelLeft + pixelToolbarWidth) -
                 qRound((logicalLeft + logicalToolbarWidth) * density)) <= 1);
}

void TestPlankToolbarLogic::preservesHorizontalPositionAcrossScaleChanges()
{
    constexpr int toolbarWidth = 539;
    constexpr int oldWindowWidth = 3840;
    constexpr int oldLeft = 2476;
    constexpr int newWindowWidth = 3072;
    const float position = PlankToolbarLogic::horizontalPosition(
                oldLeft, oldWindowWidth, toolbarWidth);
    const int newLeft = PlankToolbarLogic::logicalLeftFromPosition(
                position, newWindowWidth, toolbarWidth);

    QVERIFY(qAbs(PlankToolbarLogic::horizontalPosition(
                     newLeft, newWindowWidth, toolbarWidth) - position) < 0.001f);
}

void TestPlankToolbarLogic::isolatesVisibleNativeChildFromParentMotion()
{
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, false));
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
}

void TestPlankToolbarLogic::keepsNativeButtonSequenceOutOfSdlPath()
{
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, true));
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, true));
}

void TestPlankToolbarLogic::keepsRemotePressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, false), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(1, false, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Local);
}

void TestPlankToolbarLogic::keepsLocalPressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, true), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(1, false, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Remote);
}

void TestPlankToolbarLogic::keepsMultiButtonSequenceWithFirstOwner()
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

void TestPlankToolbarLogic::confinesToVideoWithoutToolbar()
{
    const PlankPointerLogic::Rect window = {0, 0, 3840, 2160};
    const PlankPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = PlankPointerLogic::pointerConfinementRect(
                window, video, false);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 228);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 1704);
}

void TestPlankToolbarLogic::reachesWindowEdgeWithToolbar()
{
    const PlankPointerLogic::Rect window = {0, 0, 3840, 2160};
    const PlankPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = PlankPointerLogic::pointerConfinementRect(
                window, video, true);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 0);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 2160);
}

void TestPlankToolbarLogic::calculatesOnePreFecLossInterval()
{
    VideoPacketLossInterval interval;
    QVERIFY(!interval.addCumulative(1000, 10).has_value());

    const auto clean = interval.addCumulative(2000, 10);
    QVERIFY(clean.has_value());
    QCOMPARE(*clean, 0.0f);

    const auto loss = interval.addCumulative(3000, 35);
    QVERIFY(loss.has_value());
    QCOMPARE(*loss, 2.5f);
}

void TestPlankToolbarLogic::resetsPreFecLossAfterCounterRestart()
{
    VideoPacketLossInterval interval;
    QVERIFY(!interval.addCumulative(1000, 10).has_value());
    QVERIFY(interval.addCumulative(2000, 20).has_value());
    QVERIFY(!interval.addCumulative(50, 1).has_value());

    const auto restarted = interval.addCumulative(150, 3);
    QVERIFY(restarted.has_value());
    QCOMPARE(*restarted, 2.0f);
    QVERIFY(!interval.addCumulative(100, 101).has_value());
}

void TestPlankToolbarLogic::retainsTenSecondPeakForBothStatsViews()
{
    VideoPacketLossPeakWindow window;
    QCOMPARE(window.addSample(1000, 0.5f), 0.5f);
    QCOMPARE(window.addSample(5000, 4.0f), 4.0f);
    QCOMPARE(window.addSample(10999, 0.0f), 4.0f);
    QCOMPARE(window.addSample(15000, 1.0f), 1.0f);
    window.reset();
    QCOMPARE(window.addSample(15001, 0.0f), 0.0f);
}

void TestPlankToolbarLogic::sharesPacketLossDisplayPrecision()
{
    QCOMPARE(VideoPacketLossDisplayDecimalPlaces, 2);
}

QTEST_APPLESS_MAIN(TestPlankToolbarLogic)

#include "test_planktoolbarlogic.moc"
