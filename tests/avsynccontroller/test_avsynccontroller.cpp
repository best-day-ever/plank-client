#include <QtTest>

#include "avsynccontroller.h"

#include <cmath>

class TestAvSyncController : public QObject
{
    Q_OBJECT

private slots:
    void leavesMatchedClocksUnchanged();
    void correctsFastAudioClock();
    void correctsSlowAudioClock();
    void boundsExtremeCorrection();
    void ignoresStaleVideoClock();
};

namespace {
int runClockModel(double audioPpm, int durationSeconds)
{
    StationConnectAvSync::AudioRateController controller;
    for (int second = 0; second <= durationSeconds; second++) {
        const auto audioFrames = static_cast<std::uint64_t>(std::llround(
            second * 48000.0 * (1.0 + audioPpm / 1000000.0)));
        const StationConnectAvSync::VideoClockSample video {
            second * 1000LL,
            static_cast<std::uint32_t>(second * 1000),
            true
        };
        controller.update(audioFrames,
                          48000,
                          static_cast<std::uint32_t>(second * 1000 + 20),
                          video);
    }
    return controller.correctionPpm();
}
}

void TestAvSyncController::leavesMatchedClocksUnchanged()
{
    QCOMPARE(runClockModel(0.0, 180), 0);
}

void TestAvSyncController::correctsFastAudioClock()
{
    const int correction = runClockModel(30.0, 180);
    QVERIFY(correction >= 28);
    QVERIFY(correction <= 32);
}

void TestAvSyncController::correctsSlowAudioClock()
{
    const int correction = runClockModel(-45.0, 180);
    QVERIFY(correction >= -47);
    QVERIFY(correction <= -43);
}

void TestAvSyncController::boundsExtremeCorrection()
{
    QCOMPARE(runClockModel(1000.0, 300),
             StationConnectAvSync::AudioRateController::MaximumCorrectionPpm);
    QCOMPARE(runClockModel(-1000.0, 300),
             -StationConnectAvSync::AudioRateController::MaximumCorrectionPpm);
}

void TestAvSyncController::ignoresStaleVideoClock()
{
    StationConnectAvSync::AudioRateController controller;
    const StationConnectAvSync::VideoClockSample current {0, 1000, true};
    const auto initial = controller.update(0, 48000, 1020, current);
    QVERIFY(!initial.updated);

    const StationConnectAvSync::VideoClockSample stale {1000, 2000, true};
    const auto ignored = controller.update(48000, 48000, 5001, stale);
    QVERIFY(!ignored.updated);
    QCOMPARE(ignored.correctionPpm, 0);
}

QTEST_APPLESS_MAIN(TestAvSyncController)
#include "test_avsynccontroller.moc"
