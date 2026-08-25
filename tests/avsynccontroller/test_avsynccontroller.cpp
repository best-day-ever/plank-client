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
    void boundsLongRunPhaseError();
    void calculatesNativeAudioFrequencyRatio();
    void leavesSmallAudioBacklogUnchanged();
    void catchesUpBoundedAudioBacklog();
    void removesCatchUpAfterBacklogDrains();
};

namespace {
int runClockModel(double audioPpm, int durationSeconds)
{
    StationConnectAvSync::AudioRateController controller;
    double submittedFrames = 0.0;
    std::uint64_t previousRawFrames = 0;
    for (int second = 0; second <= durationSeconds; second++) {
        const auto audioFrames = static_cast<std::uint64_t>(std::llround(
            second * 48000.0 * (1.0 + audioPpm / 1000000.0)));
        if (second != 0) {
            submittedFrames += (audioFrames - previousRawFrames) *
                (1.0 - controller.correctionPpm() / 1000000.0);
        }
        previousRawFrames = audioFrames;
        const StationConnectAvSync::VideoClockSample video {
            second * 1000LL,
            static_cast<std::uint32_t>(second * 1000),
            true
        };
        controller.update(audioFrames,
                          static_cast<std::uint64_t>(std::llround(submittedFrames)),
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
    QVERIFY(correction >= -50);
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
    const auto initial = controller.update(0, 0, 48000, 1020, current);
    QVERIFY(!initial.updated);

    const StationConnectAvSync::VideoClockSample stale {1000, 2000, true};
    const auto ignored = controller.update(48000, 48000, 48000, 5001, stale);
    QVERIFY(!ignored.updated);
    QCOMPARE(ignored.correctionPpm, 0);
}

void TestAvSyncController::boundsLongRunPhaseError()
{
    StationConnectAvSync::AudioRateController controller;
    constexpr double AudioPpm = 80.0;
    constexpr double VideoPpm = 3.0;
    double submittedFrames = 0.0;
    std::uint64_t previousRawFrames = 0;
    double relativePhaseErrorMs = 0.0;
    for (int second = 0; second <= 7200; second++) {
        const auto rawFrames = static_cast<std::uint64_t>(std::llround(
            second * 48000.0 * (1.0 + AudioPpm / 1000000.0)));
        if (second != 0) {
            submittedFrames += (rawFrames - previousRawFrames) *
                (1.0 - controller.correctionPpm() / 1000000.0);
        }
        previousRawFrames = rawFrames;
        const auto submitted = static_cast<std::uint64_t>(
            std::llround(submittedFrames));
        const auto videoMediaMs = static_cast<std::int64_t>(std::llround(
            second * 1000.0 * (1.0 + VideoPpm / 1000000.0)));
        controller.update(rawFrames,
                          submitted,
                          48000,
                          static_cast<std::uint32_t>(second * 1000 + 20),
                          {videoMediaMs,
                           static_cast<std::uint32_t>(second * 1000),
                           true});
        relativePhaseErrorMs =
            second * 1000.0 - submitted * 1000.0 / 48000.0 -
            (second * 1000.0 - videoMediaMs);
    }
    QVERIFY(std::abs(relativePhaseErrorMs) < 20.0);
}

void TestAvSyncController::calculatesNativeAudioFrequencyRatio()
{
    const float unchanged =
        StationConnectAvSync::calculateAudioFrequencyRatio(0);
    QCOMPARE(unchanged, 1.0f);

    const float faster =
        StationConnectAvSync::calculateAudioFrequencyRatio(22);
    QVERIFY(faster > 1.0f);
    QVERIFY(std::abs(faster - 1.000022f) < 0.0000001f);

    const float slower =
        StationConnectAvSync::calculateAudioFrequencyRatio(-22);
    QVERIFY(slower < 1.0f);
    QVERIFY(std::abs(slower - 0.999978f) < 0.0000001f);

    const float backlogCatchup =
        StationConnectAvSync::calculateAudioFrequencyRatio(10000);
    QVERIFY(std::abs(backlogCatchup - 1.01f) < 0.0000001f);
}

void TestAvSyncController::leavesSmallAudioBacklogUnchanged()
{
    StationConnectAvSync::AudioBacklogController controller;
    for (std::uint32_t ticks = 0; ticks <= 1000; ticks += 100) {
        controller.update(15, ticks);
    }
    QCOMPARE(controller.correctionPpm(), 0);
}

void TestAvSyncController::catchesUpBoundedAudioBacklog()
{
    StationConnectAvSync::AudioBacklogController controller;
    for (std::uint32_t ticks = 0; ticks <= 1000; ticks += 100) {
        controller.update(35, ticks);
    }
    QCOMPARE(controller.correctionPpm(), 10000);
}

void TestAvSyncController::removesCatchUpAfterBacklogDrains()
{
    StationConnectAvSync::AudioBacklogController controller;
    for (std::uint32_t ticks = 0; ticks <= 1000; ticks += 100) {
        controller.update(35, ticks);
    }
    for (std::uint32_t ticks = 1100; ticks <= 2100; ticks += 100) {
        controller.update(0, ticks);
    }
    QCOMPARE(controller.correctionPpm(), 0);
}

QTEST_APPLESS_MAIN(TestAvSyncController)
#include "test_avsynccontroller.moc"
